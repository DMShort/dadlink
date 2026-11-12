use serde::{Deserialize, Serialize};

/// Envelope for all WebSocket messages.
#[derive(Debug, Serialize, Deserialize)]
pub struct Envelope {
    #[serde(rename = "type")]
    pub msg_type: String,
    pub payload: serde_json::Value,
}

// ─── Client → Server ───

#[derive(Debug, Deserialize)]
pub struct AuthenticatePayload {
    pub method: String, // "password" or "token"
    pub username: Option<String>,
    pub password: Option<String>,
    pub token: Option<String>,
    pub org_tag: String,
}

#[derive(Debug, Deserialize)]
pub struct JoinChannelPayload {
    pub channel_id: i32,
    pub password: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct LeaveChannelPayload {
    pub channel_id: i32,
}

#[derive(Debug, Deserialize)]
pub struct SetMutedPayload {
    pub muted: bool,
}

#[derive(Debug, Deserialize)]
pub struct SetDeafenedPayload {
    pub deafened: bool,
}

#[derive(Debug, Deserialize)]
pub struct KeyExchangeResponsePayload {
    pub public_key: String, // base64-encoded X25519 public key
}

// ─── Server → Client ───

#[derive(Debug, Serialize)]
pub struct ChallengePayload {
    pub methods: Vec<String>,
}

#[derive(Debug, Serialize)]
pub struct AuthSuccessPayload {
    pub user_id: i32,
    pub token: String,
    pub voice_port: u16,
    pub channels: Vec<ChannelInfo>,
    pub users: Vec<UserInfo>,
    /// Lets the client decide which menus to show (the server still enforces everything).
    pub is_admin: bool,
    pub permissions: i32,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub client_version: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub client_download_url: Option<String>,
}

#[derive(Debug, Serialize)]
pub struct AuthFailedPayload {
    pub reason: String,
}

#[derive(Debug, Serialize)]
pub struct ChannelJoinedPayload {
    pub channel_id: i32,
    pub roster: Vec<UserInfo>,
    /// False when the user is listen-only in this channel (ACL / op net access).
    pub can_speak: bool,
}

#[derive(Debug, Serialize, Clone)]
pub struct UserJoinedPayload {
    pub channel_id: i32,
    pub user: UserInfo,
}

#[derive(Debug, Serialize, Clone)]
pub struct UserLeftPayload {
    pub channel_id: i32,
    pub user_id: i32,
}

#[derive(Debug, Serialize, Clone)]
pub struct UserStateChangedPayload {
    pub channel_id: i32,
    pub user_id: i32,
    pub muted: bool,
    pub deafened: bool,
    pub talking: bool,
}

#[derive(Debug, Serialize)]
pub struct ChannelListPayload {
    pub channels: Vec<ChannelInfo>,
}

#[derive(Debug, Serialize, Clone)]
pub struct ChannelUpdatedPayload {
    pub channel: ChannelInfo,
}

#[derive(Debug, Serialize, Clone)]
pub struct ChannelDeletedPayload {
    pub channel_id: i32,
}

#[derive(Debug, Serialize, Clone)]
pub struct UserCountChangedPayload {
    pub channel_id: i32,
    pub user_count: i32,
}

#[derive(Debug, Serialize, Clone)]
pub struct ErrorPayload {
    pub code: String,
    pub message: String,
}

#[derive(Debug, Serialize)]
pub struct PongPayload {}

#[derive(Debug, Serialize)]
pub struct KeyExchangeInitPayload {
    pub public_key: String, // base64-encoded
}

#[derive(Debug, Serialize)]
pub struct KeyExchangeCompletePayload {}

// ─── Shared types ───

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct ChannelInfo {
    pub id: i32,
    pub name: String,
    pub description: Option<String>,
    pub parent_id: Option<i32>,
    pub position: i32,
    pub max_users: i32,
    pub has_password: bool,
    pub user_count: i32,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct UserInfo {
    pub id: i32,
    pub username: String,
    pub display_name: Option<String>,
    pub muted: bool,
    pub deafened: bool,
    pub talking: bool,
}

// ─── Helpers ───

impl Envelope {
    pub fn new<T: Serialize>(msg_type: &str, payload: &T) -> Self {
        Self {
            msg_type: msg_type.to_string(),
            payload: serde_json::to_value(payload).unwrap_or_default(),
        }
    }

    pub fn to_json(&self) -> String {
        serde_json::to_string(self).unwrap_or_default()
    }
}

pub fn make_error(code: &str, message: &str) -> String {
    Envelope::new("error", &ErrorPayload {
        code: code.to_string(),
        message: message.to_string(),
    }).to_json()
}
