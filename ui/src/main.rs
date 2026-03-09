use dioxus::prelude::*;
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex};

mod components;
mod helpers;
mod network;
mod types;

use components::*;
use helpers::*;
use types::*;

static CSS: Asset = asset!("/assets/main.css");

// ============================================================
// Entry point
// ============================================================

fn main() {
    dioxus::launch(App);
}

// ============================================================
// Root App component
// ============================================================

#[component]
fn App() -> Element {
    let connection_status = use_signal(|| ConnectionStatus::Disconnected);
    let rooms: Signal<Vec<ChatRoom>> = use_signal(Vec::new);
    let messages: Signal<Vec<ChatMessage>> = use_signal(Vec::new);
    let active_room_id: Signal<Option<u64>> = use_signal(|| None);
    let toasts: Signal<Vec<(String, String, u64)>> = use_signal(Vec::new);
    let toast_counter = use_signal(|| 0u64);

    // Channels for communication with the TCP task
    let cmd_tx: Signal<Option<mpsc::Sender<ToServer>>> = use_signal(|| None);
    let event_rx: Signal<Option<Arc<Mutex<mpsc::Receiver<FromServer>>>>> = use_signal(|| None);

    // Poll events from the TCP task
    let _event_poller = use_resource(move || {
        let mut rooms = rooms;
        let mut messages = messages;
        let mut connection_status = connection_status;
        let mut toasts = toasts;
        let mut toast_counter = toast_counter;
        let mut active_room_id = active_room_id;

        async move {
            loop {
                tokio::time::sleep(tokio::time::Duration::from_millis(50)).await;

                let rx_opt = event_rx();
                if rx_opt.is_none() {
                    continue;
                }
                let rx_arc = rx_opt.unwrap();
                let msg = {
                    let mut rx = rx_arc.lock().await;
                    rx.try_recv().ok()
                };

                if let Some(event) = msg {
                    match event {
                        FromServer::Connected => {
                            *connection_status.write() = ConnectionStatus::Connected;
                            add_toast(
                                &mut toasts,
                                &mut toast_counter,
                                "success",
                                "Connected to server!".to_string(),
                            );
                        }
                        FromServer::RoomConfirmed {
                            server_room_id,
                            room_name,
                            is_create,
                        } => {
                            // The server just confirmed a sub or csub.
                            // We need to reconcile local room state with
                            // the server's authoritative id / name.
                            let mut r = rooms.write();

                            if is_create {
                                // For csub_ok we match by name (the user
                                // typed the name locally before the server
                                // assigned an id).
                                if let Some(room) = r.iter_mut().find(|rm| rm.name == room_name) {
                                    let old_id = room.id;
                                    if old_id != server_room_id {
                                        room.id = server_room_id;
                                        // Re-key every message that used the
                                        // old local id.
                                        let mut msgs = messages.write();
                                        for m in msgs.iter_mut() {
                                            if m.room_id == old_id {
                                                m.room_id = server_room_id;
                                            }
                                        }
                                        // Fix active selection if it pointed
                                        // at the old id.
                                        if active_room_id() == Some(old_id) {
                                            *active_room_id.write() = Some(server_room_id);
                                        }
                                    }
                                }
                            } else {
                                // For sub_ok we match by the id the user
                                // typed (which equals server_room_id) and
                                // update the display name.
                                if let Some(room) = r.iter_mut().find(|rm| rm.id == server_room_id)
                                {
                                    room.name = room_name.clone();
                                }
                            }
                        }
                        FromServer::Message { group_name, text } => {
                            // Find the room by name
                            let room_id = {
                                let r = rooms.read();
                                r.iter().find(|rm| rm.name == group_name).map(|rm| rm.id)
                            };

                            if let Some(rid) = room_id {
                                let chat_msg = ChatMessage {
                                    room_id: rid,
                                    text: text.clone(),
                                    sender: MessageSender::Server,
                                    timestamp: current_time_str(),
                                };
                                messages.write().push(chat_msg);

                                // Bump unread if not active
                                let current_active = active_room_id();
                                if current_active != Some(rid) {
                                    let mut r = rooms.write();
                                    if let Some(room) = r.iter_mut().find(|rm| rm.id == rid) {
                                        room.unread += 1;
                                    }
                                }
                            }
                        }
                        FromServer::Error(e) => {
                            add_toast(&mut toasts, &mut toast_counter, "error", e);
                        }
                        FromServer::Disconnected => {
                            *connection_status.write() = ConnectionStatus::Disconnected;
                            add_toast(
                                &mut toasts,
                                &mut toast_counter,
                                "info",
                                "Disconnected from server".to_string(),
                            );
                        }
                    }
                }
            }
        }
    });

    // Provide shared state through context
    use_context_provider(|| connection_status);
    use_context_provider(|| rooms);
    use_context_provider(|| messages);
    use_context_provider(|| active_room_id);
    use_context_provider(|| cmd_tx);
    use_context_provider(|| event_rx);
    use_context_provider(|| toasts);
    use_context_provider(|| toast_counter);

    let status = connection_status();
    let is_connected = matches!(status, ConnectionStatus::Connected);

    rsx! {
        document::Stylesheet { href: CSS }

        // Toasts
        ToastContainer {}

        if is_connected {
            div { class: "app-container",
                Sidebar {}
                ChatPanel {}
            }
        } else {
            ConnectScreen {}
        }
    }
}
