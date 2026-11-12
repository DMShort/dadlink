pub mod admin;
pub mod discord;
pub mod download;
pub mod health;
pub mod messages;
pub mod metrics;
pub mod rate_limit;
pub mod register;
pub mod web_admin;
pub mod websocket;

use std::sync::Arc;

use axum::{routing::{get, post}, Router};
use sqlx::PgPool;
use tokio::time::Instant;

use crate::auth::session::SessionCache;
use crate::config::AppConfig;
use crate::crypto::session_manager::SrtpSessionManager;
use crate::state::channel_manager::ChannelManager;
use crate::state::connection_manager::ConnectionManager;
use crate::state::presence::PresenceManager;

/// Shared application state available to all handlers.
#[derive(Clone)]
pub struct AppState {
    pub pool: PgPool,
    pub config: Arc<AppConfig>,
    pub session_cache: Arc<SessionCache>,
    pub channel_manager: Arc<ChannelManager>,
    pub connection_manager: Arc<ConnectionManager>,
    pub presence: Arc<PresenceManager>,
    pub srtp_manager: Arc<SrtpSessionManager>,
    pub start_time: Instant,
}

pub fn build_router(state: AppState) -> Router {
    Router::new()
        .merge(health::routes())
        .merge(metrics::routes())
        .merge(admin::admin_routes())
        .merge(web_admin::web_admin_routes())
        .merge(download::routes())
        .merge(discord::routes())
        .route("/register", post(register::register))
        .route("/ws", get(websocket::ws_handler))
        .with_state(state)
}
