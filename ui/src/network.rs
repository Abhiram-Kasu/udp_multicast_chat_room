use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::TcpStream;
use tokio::sync::{mpsc, Mutex};

use crate::types::{FromServer, ServerIncoming, ToServer};

/// Long-running async task that manages the TCP connection to the chat server.
///
/// It reads newline-delimited JSON from the server and forwards parsed events
/// through `event_tx`. Commands from the UI arrive via `cmd_rx` and are
/// serialised as JSON + newline before being written to the socket.
pub async fn tcp_connection_task(
    host: String,
    port: u16,
    cmd_rx: Arc<Mutex<mpsc::Receiver<ToServer>>>,
    event_tx: mpsc::Sender<FromServer>,
) {
    let addr = format!("{}:{}", host, port);
    let stream = match TcpStream::connect(&addr).await {
        Ok(s) => s,
        Err(e) => {
            let _ = event_tx
                .send(FromServer::Error(format!("Connection failed: {}", e)))
                .await;
            let _ = event_tx.send(FromServer::Disconnected).await;
            return;
        }
    };

    // Signal that we are connected
    let _ = event_tx.send(FromServer::Connected).await;

    let (reader, writer) = stream.into_split();
    let writer = Arc::new(Mutex::new(writer));
    let mut buf_reader = BufReader::new(reader);

    // Spawn a task to read from the server
    let event_tx_read = event_tx.clone();
    let read_handle = tokio::spawn(async move {
        let mut line = String::new();
        loop {
            line.clear();
            match buf_reader.read_line(&mut line).await {
                Ok(0) => {
                    let _ = event_tx_read.send(FromServer::Disconnected).await;
                    break;
                }
                Ok(_) => {
                    let trimmed = line.trim();
                    if trimmed.is_empty() {
                        continue;
                    }
                    // Try to parse as JSON
                    if let Ok(msg) = serde_json::from_str::<ServerIncoming>(trimmed) {
                        // Dispatch based on message type
                        let event = classify_server_message(&msg, trimmed);
                        let _ = event_tx_read.send(event).await;
                    } else {
                        // Plain text response from server (error messages, etc.)
                        let _ = event_tx_read
                            .send(FromServer::Error(trimmed.to_string()))
                            .await;
                    }
                }
                Err(e) => {
                    let _ = event_tx_read
                        .send(FromServer::Error(format!("Read error: {}", e)))
                        .await;
                    let _ = event_tx_read.send(FromServer::Disconnected).await;
                    break;
                }
            }
        }
    });

    // Process commands from the UI
    loop {
        let cmd = {
            let mut rx = cmd_rx.lock().await;
            rx.recv().await
        };
        match cmd {
            Some(ToServer::Subscribe { room_id }) => {
                let json = serde_json::json!({
                    "type": "sub",
                    "grp": room_id,
                });
                let msg = format!("{}\n", json);
                let mut w = writer.lock().await;
                if w.write_all(msg.as_bytes()).await.is_err() {
                    break;
                }
                let _ = w.flush().await;
            }
            Some(ToServer::CreateAndSubscribe { room_name }) => {
                let json = serde_json::json!({
                    "type": "csub",
                    "grp_name": room_name,
                });
                let msg = format!("{}\n", json);
                let mut w = writer.lock().await;
                if w.write_all(msg.as_bytes()).await.is_err() {
                    break;
                }
                let _ = w.flush().await;
            }
            Some(ToServer::SendMessage { room_id, message }) => {
                let json = serde_json::json!({
                    "type": "msg",
                    "grp": room_id,
                    "msg": message,
                });
                let msg = format!("{}\n", json);
                let mut w = writer.lock().await;
                if w.write_all(msg.as_bytes()).await.is_err() {
                    break;
                }
                let _ = w.flush().await;
            }
            Some(ToServer::Disconnect) => {
                let json = serde_json::json!({
                    "type": "desub",
                });
                let msg = format!("{}\n", json);
                let mut w = writer.lock().await;
                let _ = w.write_all(msg.as_bytes()).await;
                let _ = w.flush().await;
                break;
            }
            None => break,
        }
    }

    read_handle.abort();
    let _ = event_tx.send(FromServer::Disconnected).await;
}

/// Inspect a parsed `ServerIncoming` and turn it into the appropriate
/// `FromServer` variant.
///
/// The server sends several JSON shapes:
///
/// 1. **sub_ok / csub_ok** – subscription confirmation
///    `{"type":"sub_ok","grp":1,"grp_name":"room_name"}`
///    `{"type":"csub_ok","grp":1,"grp_name":"room_name"}`
///
/// 2. **message** – relayed chat message from `spawn_listener`
///    `{"type":"message","data":"hello","grp_name":"room_name"}`
///
/// 3. **legacy broadcast** (should no longer arrive after the server fix, but
///    kept for backwards compat)
///    `{"grp":"room_name","message":"hello"}`
fn classify_server_message(msg: &ServerIncoming, raw: &str) -> FromServer {
    match msg.msg_type.as_deref() {
        // ── Subscription confirmations ──────────────────────────────
        Some("sub_ok") | Some("csub_ok") => {
            let is_create = msg.msg_type.as_deref() == Some("csub_ok");
            let server_room_id = match msg.grp_as_u64() {
                Some(id) => id,
                None => {
                    return FromServer::Error(format!(
                        "sub_ok/csub_ok missing numeric grp: {}",
                        raw
                    ));
                }
            };
            let room_name = msg
                .grp_name
                .clone()
                .unwrap_or_else(|| format!("Room #{}", server_room_id));

            FromServer::RoomConfirmed {
                server_room_id,
                room_name,
                is_create,
            }
        }

        // ── Relayed chat message from spawn_listener ────────────────
        Some("message") => {
            let group_name = msg
                .grp_name
                .clone()
                .unwrap_or_else(|| "unknown".to_string());
            let text = msg.data.clone().unwrap_or_else(|| raw.to_string());

            FromServer::Message { group_name, text }
        }

        // ── Any other typed message we don't recognise ──────────────
        Some(_unknown) => FromServer::Error(raw.to_string()),

        // ── No "type" field: legacy broadcast format ────────────────
        None => {
            let group_name = msg
                .grp_as_str()
                .map(String::from)
                .or_else(|| msg.grp_name.clone())
                .unwrap_or_else(|| "unknown".to_string());
            let text = msg
                .message
                .clone()
                .or_else(|| msg.data.clone())
                .unwrap_or_else(|| raw.to_string());

            FromServer::Message { group_name, text }
        }
    }
}
