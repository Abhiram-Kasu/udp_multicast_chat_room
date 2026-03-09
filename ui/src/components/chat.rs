use dioxus::prelude::*;
use tokio::sync::mpsc;

use crate::helpers::{add_toast, avatar_color_class, current_time_str, room_initials};
use crate::types::{ChatMessage, ChatRoom, MessageSender, ToServer};

// ============================================================
// Chat Panel (main area)
// ============================================================

#[component]
pub fn ChatPanel() -> Element {
    let active_room_id = use_context::<Signal<Option<u64>>>();
    let rooms = use_context::<Signal<Vec<ChatRoom>>>();

    let active_id = active_room_id();
    let active_room = active_id.and_then(|id| {
        let r = rooms();
        r.into_iter().find(|rm| rm.id == id)
    });

    rsx! {
        div { class: "chat-area",
            if let Some(room) = active_room {
                ChatHeader { room_id: room.id, room_name: room.name.clone() }
                MessageList { room_id: room.id }
                MessageInput { room_id: room.id, room_name: room.name.clone() }
            } else {
                NoRoomSelected {}
            }
        }
    }
}

// ============================================================
// No Room Selected
// ============================================================

#[component]
fn NoRoomSelected() -> Element {
    rsx! {
        div { class: "no-room-selected",
            div { class: "icon", "💬" }
            h2 { "Welcome to Multicast Chat" }
            p {
                "Select a room from the sidebar to start chatting, or create a new room to get the conversation going."
            }
            div { class: "flex gap-3",
                div { class: "chip accent", "Create rooms with +" }
                div { class: "chip success", "Subscribe by ID with →" }
            }
        }
    }
}

// ============================================================
// Chat Header
// ============================================================

#[component]
fn ChatHeader(room_id: u64, room_name: String) -> Element {
    let mut rooms = use_context::<Signal<Vec<ChatRoom>>>();
    let mut messages = use_context::<Signal<Vec<ChatMessage>>>();
    let mut active_room_id = use_context::<Signal<Option<u64>>>();
    let mut toasts = use_context::<Signal<Vec<(String, String, u64)>>>();
    let mut toast_counter = use_context::<Signal<u64>>();

    let color_class = avatar_color_class(room_id);
    let initials = room_initials(&room_name);

    rsx! {
        div { class: "chat-header",
            div { class: "chat-header-info",
                div { class: "room-avatar {color_class}", "{initials}" }
                div {
                    div { class: "chat-header-title", "{room_name}" }
                    div { class: "chat-header-subtitle", "Room ID: {room_id}" }
                }
            }
            div { class: "chat-header-actions",
                button {
                    class: "btn btn-danger btn-sm",
                    onclick: move |_| {
                        let name = room_name.clone();
                        rooms.write().retain(|rm| rm.id != room_id);
                        messages.write().retain(|m| m.room_id != room_id);
                        *active_room_id.write() = None;
                        add_toast(
                            &mut toasts,
                            &mut toast_counter,
                            "info",
                            format!("Unsubscribed from \"{}\"", name),
                        );
                    },
                    "Unsubscribe"
                }
            }
        }
    }
}

// ============================================================
// Message List
// ============================================================

#[component]
fn MessageList(room_id: u64) -> Element {
    let messages = use_context::<Signal<Vec<ChatMessage>>>();
    let room_messages: Vec<ChatMessage> = messages()
        .into_iter()
        .filter(|m| m.room_id == room_id)
        .collect();

    rsx! {
        div { class: "messages-container",
            if room_messages.is_empty() {
                div { class: "empty-state",
                    div { class: "empty-icon", "🗨" }
                    h2 { "No messages yet" }
                    p { "Send the first message to this room!" }
                }
            }

            for (idx, msg) in room_messages.iter().enumerate() {
                match &msg.sender {
                    MessageSender::System => rsx! {
                        div { class: "system-message", key: "msg-{room_id}-{idx}",
                            span { "{msg.text}" }
                        }
                    },
                    MessageSender::Me => rsx! {
                        div { class: "message-group", key: "msg-{room_id}-{idx}",
                            div { class: "message-bubble sent",
                                "{msg.text}"
                            }
                            div { class: "message-meta sent",
                                "You · {msg.timestamp}"
                            }
                        }
                    },
                    MessageSender::Server => rsx! {
                        div { class: "message-group", key: "msg-{room_id}-{idx}",
                            div { class: "message-bubble received",
                                "{msg.text}"
                            }
                            div { class: "message-meta received",
                                "Remote · {msg.timestamp}"
                            }
                        }
                    },
                }
            }
        }
    }
}

// ============================================================
// Message Input
// ============================================================

#[component]
fn MessageInput(room_id: u64, room_name: String) -> Element {
    let mut input_text = use_signal(String::new);
    let cmd_tx = use_context::<Signal<Option<mpsc::Sender<ToServer>>>>();
    let mut messages = use_context::<Signal<Vec<ChatMessage>>>();

    let mut do_send = move || {
        let text = input_text().trim().to_string();
        if text.is_empty() {
            return;
        }

        // Add to local messages as "sent"
        messages.write().push(ChatMessage {
            room_id,
            text: text.clone(),
            sender: MessageSender::Me,
            timestamp: current_time_str(),
        });

        // Send to server
        if let Some(tx) = cmd_tx() {
            let tx_clone = tx.clone();
            let message = text.clone();
            tokio::spawn(async move {
                let _ = tx_clone
                    .send(ToServer::SendMessage { room_id, message })
                    .await;
            });
        }

        input_text.write().clear();
    };

    rsx! {
        div { class: "message-input-area",
            div { class: "message-input-wrapper",
                input {
                    r#type: "text",
                    placeholder: "Type a message to {room_name}...",
                    value: "{input_text}",
                    oninput: move |e| *input_text.write() = e.value(),
                    onkeydown: move |e| {
                        if e.key() == Key::Enter {
                            do_send();
                        }
                    },
                }
                button {
                    class: "send-btn",
                    disabled: input_text().trim().is_empty(),
                    onclick: move |_| do_send(),
                    "↑"
                }
            }
        }
    }
}
