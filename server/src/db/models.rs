use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use sqlx::FromRow;
use uuid::Uuid;

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct Organization {
    pub id: i32,
    pub name: String,
    pub tag: String,
    pub owner_id: Option<i32>,
    pub max_users: Option<i32>,
    pub max_channels: Option<i32>,
    pub created_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct User {
    pub id: i32,
    pub org_id: i32,
    pub username: String,
    #[serde(skip_serializing)]
    pub password_hash: String,
    pub display_name: Option<String>,
    pub is_admin: Option<bool>,
    pub is_active: Option<bool>,
    pub last_login: Option<DateTime<Utc>>,
    pub created_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct Session {
    pub id: i32,
    pub user_id: i32,
    pub token_jti: Uuid,
    pub issued_at: DateTime<Utc>,
    pub expires_at: DateTime<Utc>,
    pub revoked: Option<bool>,
    pub revoked_at: Option<DateTime<Utc>>,
    pub ip_address: Option<String>,
    pub created_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct Role {
    pub id: i32,
    pub org_id: i32,
    pub name: String,
    pub permissions: i32,
    pub priority: Option<i32>,
    pub color: Option<String>,
    pub created_at: Option<DateTime<Utc>>,
    /// Discord role that grants this role on Discord login (role sync).
    pub discord_role_id: Option<String>,
}

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct Channel {
    pub id: i32,
    pub org_id: i32,
    pub parent_id: Option<i32>,
    pub name: String,
    pub description: Option<String>,
    #[serde(skip_serializing)]
    pub password_hash: Option<String>,
    pub position: Option<i32>,
    pub max_users: Option<i32>,
    pub created_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct Invite {
    pub id: i32,
    pub org_id: i32,
    pub code: String,
    pub created_by: i32,
    pub max_uses: Option<i32>,
    pub use_count: i32,
    pub expires_at: Option<DateTime<Utc>>,
    pub created_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct AuditEntry {
    pub id: i64,
    pub org_id: Option<i32>,
    pub user_id: Option<i32>,
    pub action: String,
    pub target_type: Option<String>,
    pub target_id: Option<i32>,
    pub details: Option<serde_json::Value>,
    pub ip_address: Option<String>,
    pub created_at: Option<DateTime<Utc>>,
}
