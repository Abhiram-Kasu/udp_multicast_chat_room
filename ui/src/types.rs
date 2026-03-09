use serde::{Deserialize, Serialize};

// ============================================================
// Data types
// ============================================================

#[derive(Clone, Debug, PartialEq)]
pub struct ChatRoom {
    pub id: u64,
    pub name: String,
    pub unread: u32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ChatMessage {
    pub room_id: u64,
    pub text: String,
    pub sender: MessageSender,
    pub timestamp: String,
}

#[derive(Clone, Debug, PartialEq)]
pub enum MessageSender {
    Me,
    Server,
    System,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ConnectionStatus {
    Disconnected,
    Connecting,
    Connected,
}

#[derive(Clone, Debug)]
pub enum ToServer {
    Subscribe { room_id: u64 },
    CreateAndSubscribe { room_name: String },
    SendMessage { room_id: u64, message: String },
    Disconnect,
}

/// Flat representation of every possible JSON object the server can send.
///
/// The server emits several shapes:
///   - `{"type":"message","data":"...","grp_name":"..."}`  (relayed chat msg)
///   - `{"type":"sub_ok","grp":1,"grp_name":"..."}`        (subscribe confirmed)
///   - `{"type":"csub_ok","grp":1,"grp_name":"..."}`       (create+subscribe confirmed)
///
/// `grp` arrives as a number for sub_ok/csub_ok but could theoretically be a
/// string in other contexts, so we keep it as a generic `serde_json::Value`.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ServerIncoming {
    /// Could be a number (sub_ok / csub_ok) or a string (legacy broadcast).
    #[serde(default)]
    pub grp: Option<serde_json::Value>,

    #[serde(default)]
    pub message: Option<String>,

    #[serde(default, rename = "type")]
    pub msg_type: Option<String>,

    #[serde(default)]
    pub data: Option<String>,

    #[serde(default)]
    pub grp_name: Option<String>,
}

impl ServerIncoming {
    /// Try to extract `grp` as a u64 (used for sub_ok / csub_ok).
    pub fn grp_as_u64(&self) -> Option<u64> {
        self.grp.as_ref().and_then(|v| v.as_u64())
    }

    /// Try to extract `grp` as a string (used for legacy broadcast).
    pub fn grp_as_str(&self) -> Option<&str> {
        self.grp.as_ref().and_then(|v| v.as_str())
    }
}

#[derive(Clone, Debug)]
pub enum FromServer {
    Connected,

    /// A chat message was received for `group_name`.
    Message {
        group_name: String,
        text: String,
    },

    /// The server confirmed a subscription (sub_ok or csub_ok).
    /// `server_room_id` is the authoritative room id on the server.
    /// `room_name` is the room name the server knows.
    /// `is_create` is true when this was a csub_ok (room was freshly created).
    RoomConfirmed {
        server_room_id: u64,
        room_name: String,
        is_create: bool,
    },

    Error(String),
    Disconnected,
}
