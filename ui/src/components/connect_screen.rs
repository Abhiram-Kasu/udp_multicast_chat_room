use dioxus::prelude::*;
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex};

use crate::network::tcp_connection_task;
use crate::types::*;

#[component]
pub fn ConnectScreen() -> Element {
    let mut host = use_signal(|| "127.0.0.1".to_string());
    let mut port = use_signal(|| "4040".to_string());
    let mut error_msg: Signal<String> = use_signal(String::new);

    let mut connection_status = use_context::<Signal<ConnectionStatus>>();
    let mut cmd_tx_sig = use_context::<Signal<Option<mpsc::Sender<ToServer>>>>();
    let mut event_rx_sig = use_context::<Signal<Option<Arc<Mutex<mpsc::Receiver<FromServer>>>>>>();
    let mut rooms = use_context::<Signal<Vec<ChatRoom>>>();
    let mut messages = use_context::<Signal<Vec<ChatMessage>>>();
    let mut active_room_id = use_context::<Signal<Option<u64>>>();

    let status = connection_status();

    rsx! {
        div { class: "connect-screen",
            div { class: "connect-card",
                div { class: "logo", "💬" }
                h1 { "Multicast Chat" }
                p { class: "description",
                    "Connect to the TCP chat server to create rooms, subscribe to groups, and chat in real-time."
                }

                div { class: "connect-form",
                    div { class: "input-group",
                        input {
                            class: "input",
                            r#type: "text",
                            placeholder: "Host address",
                            value: "{host}",
                            oninput: move |e| *host.write() = e.value(),
                        }
                        input {
                            class: "input port",
                            r#type: "text",
                            placeholder: "Port",
                            value: "{port}",
                            oninput: move |e| *port.write() = e.value(),
                        }
                    }

                    if !error_msg().is_empty() {
                        p { class: "error-text", "{error_msg}" }
                    }

                    button {
                        class: "btn btn-primary w-full",
                        disabled: matches!(status, ConnectionStatus::Connecting),
                        onclick: move |_| {
                            let host_val = host();
                            let port_val = port();
                            let port_num: u16 = match port_val.parse() {
                                Ok(p) => p,
                                Err(_) => {
                                    *error_msg.write() = "Invalid port number".to_string();
                                    return;
                                }
                            };

                            *error_msg.write() = String::new();
                            *connection_status.write() = ConnectionStatus::Connecting;

                            // Clear previous state
                            rooms.write().clear();
                            messages.write().clear();
                            *active_room_id.write() = None;

                            let (ctx, crx) = mpsc::channel::<ToServer>(64);
                            let (etx, erx) = mpsc::channel::<FromServer>(256);
                            let crx = Arc::new(Mutex::new(crx));
                            let erx = Arc::new(Mutex::new(erx));

                            *cmd_tx_sig.write() = Some(ctx);
                            *event_rx_sig.write() = Some(erx);

                            tokio::spawn(tcp_connection_task(host_val, port_num, crx, etx));
                        },

                        if matches!(status, ConnectionStatus::Connecting) {
                            div { class: "flex items-center justify-center gap-2",
                                div { class: "spinner" }
                                span { "Connecting..." }
                            }
                        } else {
                            span { "Connect to Server" }
                        }
                    }
                }
            }
        }
    }
}
