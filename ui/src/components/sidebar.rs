use dioxus::prelude::*;
use tokio::sync::mpsc;

use crate::helpers::{add_toast, avatar_color_class, current_time_str, room_initials};
use crate::types::*;

// ============================================================
// Sidebar
// ============================================================

#[component]
pub fn Sidebar() -> Element {
    let connection_status = use_context::<Signal<ConnectionStatus>>();
    let rooms = use_context::<Signal<Vec<ChatRoom>>>();
    let active_room_id = use_context::<Signal<Option<u64>>>();

    let status = connection_status();
    let rooms_list = rooms();
    let room_count = rooms_list.len();

    rsx! {
        div { class: "sidebar",
            // Header
            div { class: "sidebar-header",
                h1 {
                    span { class: "logo-icon", "💬" }
                    "Multicast Chat"
                }
                p { class: "subtitle", "TCP Chat Client" }
            }

            // Connection status
            div { class: "connection-status",
                div {
                    class: {
                        let dot_class = match status {
                            ConnectionStatus::Connected => "status-dot connected",
                            ConnectionStatus::Connecting => "status-dot connecting",
                            ConnectionStatus::Disconnected => "status-dot disconnected",
                        };
                        dot_class
                    },
                }
                span { class: "status-text",
                    match status {
                        ConnectionStatus::Connected => "Connected",
                        ConnectionStatus::Connecting => "Connecting...",
                        ConnectionStatus::Disconnected => "Disconnected",
                    }
                }
            }

            // Create room section
            CreateRoomSection {}

            // Subscribe to room section
            SubscribeRoomSection {}

            // Disconnect button
            DisconnectSection {}

            // Rooms list
            div { class: "sidebar-section rooms-list",
                div { class: "section-header",
                    h3 { "Subscribed Rooms" }
                    if room_count > 0 {
                        span { class: "badge", "{room_count}" }
                    }
                }

                div { class: "room-list",
                    if rooms_list.is_empty() {
                        div { class: "empty-state",
                            style: "padding: 20px 10px; gap: 8px;",
                            div { class: "empty-icon", style: "font-size: 32px;", "📭" }
                            p { style: "font-size: 12px;",
                                "No rooms yet. Create or subscribe to a room above."
                            }
                        }
                    }

                    for room in rooms_list.iter() {
                        RoomItem {
                            key: "{room.id}",
                            room_id: room.id,
                            room_name: room.name.clone(),
                            unread: room.unread,
                            is_active: active_room_id() == Some(room.id),
                        }
                    }
                }
            }
        }
    }
}

// ============================================================
// Create Room Section
// ============================================================

#[component]
fn CreateRoomSection() -> Element {
    let mut new_room_name = use_signal(String::new);
    let cmd_tx = use_context::<Signal<Option<mpsc::Sender<ToServer>>>>();
    let mut rooms = use_context::<Signal<Vec<ChatRoom>>>();
    let mut messages = use_context::<Signal<Vec<ChatMessage>>>();
    let mut toasts = use_context::<Signal<Vec<(String, String, u64)>>>();
    let mut toast_counter = use_context::<Signal<u64>>();

    let mut do_create = move || {
        let name = new_room_name().trim().to_string();
        if name.is_empty() {
            return;
        }

        if let Some(tx) = cmd_tx() {
            // Assign a local client-side ID (server assigns the real one, but we track locally)
            let next_id = {
                let r = rooms.read();
                r.iter().map(|rm| rm.id).max().unwrap_or(0) + 1
            };

            rooms.write().push(ChatRoom {
                id: next_id,
                name: name.clone(),
                unread: 0,
            });

            messages.write().push(ChatMessage {
                room_id: next_id,
                text: format!("Created and joined room \"{}\"", name),
                sender: MessageSender::System,
                timestamp: current_time_str(),
            });

            let tx_clone = tx.clone();
            let room_name = name.clone();
            tokio::spawn(async move {
                let _ = tx_clone
                    .send(ToServer::CreateAndSubscribe { room_name })
                    .await;
            });

            add_toast(
                &mut toasts,
                &mut toast_counter,
                "success",
                format!("Created room \"{}\"", name),
            );
        }
        new_room_name.write().clear();
    };

    rsx! {
        div { class: "sidebar-section",
            div { class: "section-header",
                h3 { "Create Room" }
            }
            div { class: "action-row",
                input {
                    class: "input sm",
                    r#type: "text",
                    placeholder: "Room name...",
                    value: "{new_room_name}",
                    oninput: move |e| *new_room_name.write() = e.value(),
                    onkeydown: move |e| {
                        if e.key() == Key::Enter {
                            do_create();
                        }
                    },
                }
                button {
                    class: "btn btn-primary btn-sm",
                    disabled: new_room_name().trim().is_empty(),
                    onclick: move |_| do_create(),
                    "+"
                }
            }
        }
    }
}

// ============================================================
// Subscribe Room Section
// ============================================================

#[component]
fn SubscribeRoomSection() -> Element {
    let mut sub_room_id = use_signal(String::new);
    let cmd_tx = use_context::<Signal<Option<mpsc::Sender<ToServer>>>>();
    let mut rooms = use_context::<Signal<Vec<ChatRoom>>>();
    let mut messages = use_context::<Signal<Vec<ChatMessage>>>();
    let mut toasts = use_context::<Signal<Vec<(String, String, u64)>>>();
    let mut toast_counter = use_context::<Signal<u64>>();

    let mut do_subscribe = move || {
        let id_str = sub_room_id().trim().to_string();
        if id_str.is_empty() {
            return;
        }

        let room_id: u64 = match id_str.parse() {
            Ok(id) => id,
            Err(_) => {
                add_toast(
                    &mut toasts,
                    &mut toast_counter,
                    "error",
                    "Room ID must be a number".to_string(),
                );
                return;
            }
        };

        // Check if already subscribed
        {
            let r = rooms.read();
            if r.iter().any(|rm| rm.id == room_id) {
                add_toast(
                    &mut toasts,
                    &mut toast_counter,
                    "error",
                    format!("Already subscribed to room #{}", room_id),
                );
                return;
            }
        }

        if let Some(tx) = cmd_tx() {
            rooms.write().push(ChatRoom {
                id: room_id,
                name: format!("Room #{}", room_id),
                unread: 0,
            });

            messages.write().push(ChatMessage {
                room_id,
                text: format!("Subscribed to room #{}", room_id),
                sender: MessageSender::System,
                timestamp: current_time_str(),
            });

            let tx_clone = tx.clone();
            tokio::spawn(async move {
                let _ = tx_clone.send(ToServer::Subscribe { room_id }).await;
            });

            add_toast(
                &mut toasts,
                &mut toast_counter,
                "success",
                format!("Subscribed to room #{}", room_id),
            );
        }
        sub_room_id.write().clear();
    };

    rsx! {
        div { class: "sidebar-section",
            div { class: "section-header",
                h3 { "Subscribe by ID" }
            }
            div { class: "action-row",
                input {
                    class: "input sm",
                    r#type: "text",
                    placeholder: "Room ID (e.g. 1)",
                    value: "{sub_room_id}",
                    oninput: move |e| *sub_room_id.write() = e.value(),
                    onkeydown: move |e| {
                        if e.key() == Key::Enter {
                            do_subscribe();
                        }
                    },
                }
                button {
                    class: "btn btn-secondary btn-sm",
                    disabled: sub_room_id().trim().is_empty(),
                    onclick: move |_| do_subscribe(),
                    "→"
                }
            }
        }
    }
}

// ============================================================
// Disconnect Section
// ============================================================

#[component]
fn DisconnectSection() -> Element {
    let cmd_tx = use_context::<Signal<Option<mpsc::Sender<ToServer>>>>();
    let mut connection_status = use_context::<Signal<ConnectionStatus>>();

    rsx! {
        div { class: "sidebar-section",
            div { style: "padding: 8px 16px 12px;",
                button {
                    class: "btn btn-danger btn-sm w-full",
                    onclick: move |_| {
                        if let Some(tx) = cmd_tx() {
                            let tx_clone = tx.clone();
                            tokio::spawn(async move {
                                let _ = tx_clone.send(ToServer::Disconnect).await;
                            });
                        }
                        *connection_status.write() = ConnectionStatus::Disconnected;
                    },
                    "⏻  Disconnect"
                }
            }
        }
    }
}

// ============================================================
// Room Item
// ============================================================

#[component]
fn RoomItem(room_id: u64, room_name: String, unread: u32, is_active: bool) -> Element {
    let mut active_room_id = use_context::<Signal<Option<u64>>>();
    let mut rooms = use_context::<Signal<Vec<ChatRoom>>>();
    let mut messages = use_context::<Signal<Vec<ChatMessage>>>();
    let mut toasts = use_context::<Signal<Vec<(String, String, u64)>>>();
    let mut toast_counter = use_context::<Signal<u64>>();

    let active_class = if is_active {
        "room-item active"
    } else {
        "room-item"
    };
    let color_class = avatar_color_class(room_id);
    let initials = room_initials(&room_name);

    rsx! {
        div {
            class: "{active_class}",
            onclick: move |_| {
                *active_room_id.write() = Some(room_id);
                // Clear unread
                let mut r = rooms.write();
                if let Some(room) = r.iter_mut().find(|rm| rm.id == room_id) {
                    room.unread = 0;
                }
            },

            div { class: "room-info",
                div { class: "room-avatar {color_class}", "{initials}" }
                div { class: "room-details",
                    div { class: "room-name", "{room_name}" }
                    div { class: "room-id", "ID: {room_id}" }
                }
            }

            div { class: "flex items-center gap-2",
                if unread > 0 {
                    div { class: "room-unread", "{unread}" }
                }

                div { class: "room-actions",
                    button {
                        class: "btn btn-icon btn-danger sm",
                        title: "Unsubscribe from room",
                        onclick: move |evt| {
                            evt.stop_propagation();

                            // Remove from local list
                            let name = {
                                let r = rooms.read();
                                r.iter()
                                    .find(|rm| rm.id == room_id)
                                    .map(|rm| rm.name.clone())
                                    .unwrap_or_default()
                            };
                            rooms.write().retain(|rm| rm.id != room_id);
                            messages.write().retain(|m| m.room_id != room_id);

                            // If this was the active room, clear selection
                            if active_room_id() == Some(room_id) {
                                *active_room_id.write() = None;
                            }

                            add_toast(
                                &mut toasts,
                                &mut toast_counter,
                                "info",
                                format!("Unsubscribed from \"{}\"", name),
                            );
                        },
                        "×"
                    }
                }
            }
        }
    }
}
