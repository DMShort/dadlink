use std::collections::HashSet;
use std::time::{Duration, Instant};

use axum::{
    extract::{
        ws::{Message, WebSocket},
        ConnectInfo, State, WebSocketUpgrade,
    },
    response::IntoResponse,
};
use std::net::SocketAddr;
use futures::{SinkExt, StreamExt};
use tokio::sync::mpsc;
use tracing::{debug, info, warn};
use uuid::Uuid;

use crate::auth::{jwt, password};
use crate::crypto::key_exchange;
use crate::crypto::srtp_session::SrtpSession;
use crate::db;
use crate::server::messages::*;
use crate::state::connection_manager::ConnectionHandle;

use super::AppState;

/// WebSocket upgrade handler.
pub async fn ws_handler(
    ws: WebSocketUpgrade,
    State(state): State<AppState>,
    ConnectInfo(addr): ConnectInfo<SocketAddr>,
) -> impl IntoResponse {
    let remote_ip = addr.ip().to_string();
    ws.on_upgrade(move |socket| handle_connection(socket, state, remote_ip))
}

/// Per-connection state during authentication.
struct ConnState {
    user_id: Option<i32>,
    org_id: Option<i32>,
    username: Option<String>,
    display_name: Option<String>,
    authenticated: bool,
    jti: Option<Uuid>,
    /// Server's ephemeral secret for key exchange (consumed on use).
    ke_secret: Option<x25519_dalek::EphemeralSecret>,
    /// True once the initial SRTP key exchange has completed for this connection.
    /// Subsequent channel joins do not re-key; only the periodic rotation task does.
    srtp_initialized: bool,
    /// WebSocket message rate limiter — tokens refill at 30/sec.
    ws_tokens: f64,
    ws_last_refill: Instant,
}

const WS_RATE_LIMIT: f64 = 30.0; // messages per second

async fn handle_connection(socket: WebSocket, state: AppState, remote_ip: String) {
    let (mut ws_sink, mut ws_stream) = socket.split();
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();

    // Send challenge
    let challenge = Envelope::new("challenge", &ChallengePayload {
        methods: if state.config.discord.enabled() {
            vec!["password".into(), "token".into(), "discord".into()]
        } else {
            vec!["password".into(), "token".into()]
        },
    });
    if ws_sink.send(Message::Text(challenge.to_json())).await.is_err() {
        return;
    }

    // Spawn task to forward outbound messages from channel to WebSocket
    let send_task = tokio::spawn(async move {
        while let Some(msg) = rx.recv().await {
            if ws_sink.send(Message::Text(msg)).await.is_err() {
                break;
            }
        }
    });

    let metrics = super::metrics::metrics();
    metrics.total_connections.inc();
    metrics.active_connections.inc();

    let mut conn = ConnState {
        user_id: None,
        org_id: None,
        username: None,
        display_name: None,
        authenticated: false,
        jti: None,
        ke_secret: None,
        srtp_initialized: false,
        ws_tokens: WS_RATE_LIMIT,
        ws_last_refill: Instant::now(),
    };

    let mut last_activity = Instant::now();
    let timeout = Duration::from_secs(30);

    loop {
        tokio::select! {
            msg = ws_stream.next() => {
                match msg {
                    Some(Ok(Message::Text(text))) => {
                        last_activity = Instant::now();
                        metrics.total_messages_received.inc();

                        // WebSocket rate limit check
                        let now = Instant::now();
                        let elapsed = now.duration_since(conn.ws_last_refill).as_secs_f64();
                        conn.ws_tokens = (conn.ws_tokens + elapsed * WS_RATE_LIMIT).min(WS_RATE_LIMIT);
                        conn.ws_last_refill = now;

                        if conn.ws_tokens >= 1.0 {
                            conn.ws_tokens -= 1.0;
                            handle_message(&text, &state, &tx, &mut conn, &remote_ip).await;
                        } else {
                            metrics.ws_messages_rate_limited.inc();
                            debug!("WebSocket rate limited for user {:?}", conn.user_id);
                        }
                    }
                    Some(Ok(Message::Close(_))) | None => break,
                    Some(Err(e)) => {
                        debug!("WebSocket error: {}", e);
                        break;
                    }
                    _ => {} // Binary, Ping, Pong — ignore
                }
            }
            _ = tokio::time::sleep_until(tokio::time::Instant::from_std(last_activity + timeout)) => {
                if last_activity.elapsed() >= timeout {
                    warn!("Connection timed out (no messages for 30s)");
                    let _ = tx.send(make_error("timeout", "Connection timed out"));
                    break;
                }
            }
        }
    }

    // Cleanup on disconnect
    metrics.active_connections.dec();
    if let Some(user_id) = conn.user_id {
        cleanup_connection(user_id, &state);
    }

    send_task.abort();
}

async fn handle_message(
    text: &str,
    state: &AppState,
    tx: &mpsc::UnboundedSender<String>,
    conn: &mut ConnState,
    remote_ip: &str,
) {
    let envelope: Envelope = match serde_json::from_str(text) {
        Ok(e) => e,
        Err(_) => {
            let _ = tx.send(make_error("parse_error", "Invalid JSON message"));
            return;
        }
    };

    match envelope.msg_type.as_str() {
        "authenticate" => handle_authenticate(&envelope.payload, state, tx, conn, remote_ip).await,
        "ping" => {
            let _ = tx.send(Envelope::new("pong", &PongPayload {}).to_json());
        }
        _ if !conn.authenticated => {
            let _ = tx.send(make_error("not_authenticated", "Please authenticate first"));
        }
        "join_channel" => handle_join_channel(&envelope.payload, state, tx, &mut *conn).await,
        "leave_channel" => handle_leave_channel(&envelope.payload, state, conn).await,
        "set_muted" => handle_set_muted(&envelope.payload, state, conn).await,
        "set_deafened" => handle_set_deafened(&envelope.payload, state, conn).await,
        "key_exchange_response" => handle_key_exchange_response(&envelope.payload, state, tx, conn).await,
        _ => {
            // Unknown type — ignore for forward compatibility
            debug!("Unknown message type: {}", envelope.msg_type);
        }
    }
}

async fn handle_authenticate(
    payload: &serde_json::Value,
    state: &AppState,
    tx: &mpsc::UnboundedSender<String>,
    conn: &mut ConnState,
    remote_ip: &str,
) {
    let auth: AuthenticatePayload = match serde_json::from_value(payload.clone()) {
        Ok(a) => a,
        Err(_) => {
            let _ = tx.send(make_error("invalid_payload", "Invalid authenticate payload"));
            return;
        }
    };

    // Auth rate limiting: check IP and username before attempting auth
    let limiter = super::rate_limit::auth_limiter();
    if !limiter.check_ip(remote_ip) {
        warn!("Auth rate limited by IP: {}", remote_ip);
        let _ = tx.send(Envelope::new("auth_failed", &AuthFailedPayload {
            reason: "Too many failed attempts. Please try again later.".into(),
        }).to_json());
        super::metrics::metrics().total_auth_failures.inc();
        return;
    }

    let username_for_limit = auth.username.as_deref().unwrap_or("");
    if !username_for_limit.is_empty() && !limiter.check_username(username_for_limit) {
        warn!("Auth rate limited by username: {}", username_for_limit);
        let _ = tx.send(Envelope::new("auth_failed", &AuthFailedPayload {
            reason: "Too many failed attempts. Please try again later.".into(),
        }).to_json());
        super::metrics::metrics().total_auth_failures.inc();
        return;
    }

    let result = match auth.method.as_str() {
        "password" => {
            authenticate_password(
                &state.pool,
                auth.username.as_deref().unwrap_or(""),
                auth.password.as_deref().unwrap_or(""),
                &auth.org_tag,
            )
            .await
        }
        "token" => {
            authenticate_token(
                &state.pool,
                auth.token.as_deref().unwrap_or(""),
                &state.config.security.jwt_secret,
                &state.session_cache,
            )
            .await
        }
        "discord" => authenticate_discord(&state.pool, auth.token.as_deref().unwrap_or(""), &auth.org_tag).await,
        _ => {
            let _ = tx.send(Envelope::new("auth_failed", &AuthFailedPayload {
                reason: "Unknown authentication method".into(),
            }).to_json());
            return;
        }
    };

    match result {
        Ok((user, org_id, role_ids)) => {
            // Create JWT token
            let (token, jti, issued_at, expires_at) = match jwt::create_token(
                user.id,
                org_id,
                role_ids,
                &state.config.security.jwt_secret,
                state.config.security.jwt_expiration_secs,
            ) {
                Ok(t) => t,
                Err(e) => {
                    tracing::error!("Failed to create JWT: {}", e);
                    let _ = tx.send(make_error("internal_error", "Authentication failed"));
                    return;
                }
            };

            // Store session in DB
            let _ = db::sessions::create_session(
                &state.pool, user.id, jti, issued_at, expires_at, None,
            )
            .await;

            // Update last login
            let _ = db::users::update_last_login(&state.pool, user.id).await;

            // Get channel list for the org
            let channels = db::channels::list_by_org(&state.pool, org_id)
                .await
                .unwrap_or_default();

            let channel_infos: Vec<ChannelInfo> = channels
                .iter()
                .map(|c| ChannelInfo {
                    id: c.id,
                    name: c.name.clone(),
                    description: c.description.clone(),
                    parent_id: c.parent_id,
                    position: c.position.unwrap_or(0),
                    max_users: c.max_users.unwrap_or(50),
                    has_password: c.password_hash.is_some(),
                    user_count: state.channel_manager.member_count(c.id) as i32,
                })
                .collect();

            // Register channel limits
            for ch in &channels {
                state.channel_manager.set_limit(ch.id, ch.max_users.unwrap_or(50));
            }

            // Get online users in the org
            let online_users = get_online_users_for_org(state, org_id);

            // Register connection
            let handle = ConnectionHandle {
                user_id: user.id,
                org_id,
                username: user.username.clone(),
                display_name: user.display_name.clone(),
                channels: HashSet::new(),
                tx: tx.clone(),
                udp_addr: None,
            };
            state.connection_manager.add(handle);

            // Update connection state
            conn.user_id = Some(user.id);
            conn.org_id = Some(org_id);
            conn.username = Some(user.username.clone());
            conn.display_name = user.display_name.clone();
            conn.authenticated = true;
            conn.jti = Some(jti);

            super::metrics::metrics().total_auth_successes.inc();
            info!("User '{}' authenticated (id={})", user.username, user.id);

            let role_perms = db::roles::get_user_permissions(&state.pool, user.id)
                .await
                .unwrap_or_default();
            let is_admin = user.is_admin.unwrap_or(false);
            let permissions = if is_admin {
                0x1FF
            } else {
                crate::auth::permissions::compute_permissions(&role_perms)
            };

            // Send auth success
            let _ = tx.send(Envelope::new("auth_success", &AuthSuccessPayload {
                user_id: user.id,
                token,
                voice_port: state.config.server.voice_port,
                channels: channel_infos,
                users: online_users,
                is_admin,
                permissions,
                client_version: state.config.server.client_version.clone(),
                client_download_url: state.config.server.client_download_url.clone(),
            }).to_json());

            // Ops already running in the org, personalised to this user's roles
            super::admin::ops::send_active_ops(state, user.id, org_id).await;

        }
        Err(reason) => {
            super::metrics::metrics().total_auth_failures.inc();
            // Record failures for rate limiting
            limiter.record_ip_failure(remote_ip);
            if !username_for_limit.is_empty() {
                limiter.record_username_failure(username_for_limit);
            }
            warn!("Authentication failed: {}", reason);
            let _ = tx.send(Envelope::new("auth_failed", &AuthFailedPayload {
                reason,
            }).to_json());
        }
    }
}

async fn authenticate_password(
    pool: &sqlx::PgPool,
    username: &str,
    pass: &str,
    org_tag: &str,
) -> Result<(db::models::User, i32, Vec<i32>), String> {
    let user = db::users::find_by_username_and_org(pool, username, org_tag)
        .await
        .map_err(|_| "Internal error".to_string())?
        .ok_or_else(|| "Invalid username or password".to_string())?;

    let valid = password::verify_password(pass, &user.password_hash)
        .map_err(|_| "Internal error".to_string())?;

    if !valid {
        return Err("Invalid username or password".to_string());
    }

    // Get role IDs
    let role_ids = get_user_role_ids(pool, user.id).await;

    Ok((user.clone(), user.org_id, role_ids))
}

/// Single-use ticket from the Discord browser login (see server/discord.rs).
async fn authenticate_discord(
    pool: &sqlx::PgPool,
    ticket: &str,
    org_tag: &str,
) -> Result<(db::models::User, i32, Vec<i32>), String> {
    let user_id = super::discord::redeem_ticket(ticket)
        .ok_or_else(|| "Discord login expired or already used. Please try again.".to_string())?;
    let user = db::users::find_by_id(pool, user_id)
        .await
        .map_err(|_| "Internal error".to_string())?
        .ok_or_else(|| "Account not found".to_string())?;
    if !user.is_active.unwrap_or(true) {
        return Err("Account is disabled".to_string());
    }
    // The ticket was issued for a specific org; make sure the client is logging into that one
    let (tag,): (String,) = sqlx::query_as("SELECT tag FROM organizations WHERE id = $1")
        .bind(user.org_id)
        .fetch_one(pool)
        .await
        .map_err(|_| "Internal error".to_string())?;
    if !org_tag.is_empty() && !tag.eq_ignore_ascii_case(org_tag) {
        return Err("This Discord login was for a different organisation".to_string());
    }
    let role_ids = get_user_role_ids(pool, user.id).await;
    Ok((user.clone(), user.org_id, role_ids))
}

async fn authenticate_token(
    pool: &sqlx::PgPool,
    token: &str,
    secret: &str,
    session_cache: &crate::auth::session::SessionCache,
) -> Result<(db::models::User, i32, Vec<i32>), String> {
    let claims = jwt::validate_token(token, secret)
        .map_err(|_| "Invalid or expired token".to_string())?;

    let jti = Uuid::parse_str(&claims.jti)
        .map_err(|_| "Invalid token ID".to_string())?;

    // Check session not revoked
    if !session_cache.is_session_valid(pool, jti).await {
        return Err("Session has been revoked".to_string());
    }

    let user = db::users::find_by_id(pool, claims.sub)
        .await
        .map_err(|_| "Internal error".to_string())?
        .ok_or_else(|| "User not found".to_string())?;

    if user.is_active != Some(true) {
        return Err("Account is deactivated".to_string());
    }

    Ok((user.clone(), claims.org, claims.roles))
}

async fn get_user_role_ids(pool: &sqlx::PgPool, user_id: i32) -> Vec<i32> {
    sqlx::query_scalar::<_, i32>(
        "SELECT role_id FROM user_roles WHERE user_id = $1",
    )
    .bind(user_id)
    .fetch_all(pool)
    .await
    .unwrap_or_default()
}

async fn handle_join_channel(
    payload: &serde_json::Value,
    state: &AppState,
    tx: &mpsc::UnboundedSender<String>,
    conn: &mut ConnState,
) {
    let join: JoinChannelPayload = match serde_json::from_value(payload.clone()) {
        Ok(j) => j,
        Err(_) => {
            let _ = tx.send(make_error("invalid_payload", "Invalid join_channel payload"));
            return;
        }
    };

    let (user_id, org_id) = match (conn.user_id, conn.org_id) {
        (Some(u), Some(o)) => (u, o),
        _ => return, // should never happen for authenticated connections
    };

    // Verify channel belongs to user's org
    let channel = match db::channels::get_by_id(&state.pool, join.channel_id).await {
        Ok(Some(ch)) if ch.org_id == org_id => ch,
        _ => {
            let _ = tx.send(make_error("not_found", "Channel not found"));
            return;
        }
    };

    // Check channel password if set
    if let Some(ref hash) = channel.password_hash {
        let provided = join.password.as_deref().unwrap_or("");
        match password::verify_password(provided, hash) {
            Ok(true) => {}
            _ => {
                let _ = tx.send(make_error("wrong_password", "Incorrect channel password"));
                return;
            }
        }
    }

    // Channel ACL (per-role join/speak; channels without ACL rows stay open)
    let access = match db::acl::channel_access(&state.pool, join.channel_id, user_id).await {
        Ok(a) => a,
        Err(e) => {
            warn!("ACL lookup failed for channel {}: {}", join.channel_id, e);
            let _ = tx.send(make_error("internal", "Could not check channel access"));
            return;
        }
    };
    if !access.can_join {
        let _ = tx.send(make_error("forbidden", "You don't have access to this channel"));
        return;
    }

    // Try to join (checks max_users)
    if let Err(_) = state.channel_manager.join(join.channel_id, user_id) {
        let _ = tx.send(make_error("channel_full", "Channel is full"));
        return;
    }
    state.channel_manager.set_can_speak(join.channel_id, user_id, access.can_speak);

    // Track in connection manager
    state.connection_manager.add_channel(user_id, join.channel_id);

    // Build roster of users currently in the channel
    let roster = build_channel_roster(state, join.channel_id);

    // Send channel_joined to the user
    let _ = tx.send(Envelope::new("channel_joined", &ChannelJoinedPayload {
        channel_id: join.channel_id,
        roster: roster.clone(),
        can_speak: access.can_speak,
    }).to_json());

    // Broadcast user_joined to other channel members
    let user_info = UserInfo {
        id: user_id,
        username: conn.username.clone().unwrap_or_default(),
        display_name: conn.display_name.clone(),
        muted: state.presence.get(user_id).muted,
        deafened: state.presence.get(user_id).deafened,
        talking: false,
    };

    let joined_msg = Envelope::new("user_joined", &UserJoinedPayload {
        channel_id: join.channel_id,
        user: user_info,
    }).to_json();

    state.connection_manager.broadcast_to_channel(
        join.channel_id, &joined_msg, Some(user_id),
    );

    // Broadcast updated user count to all org members
    broadcast_user_count(state, org_id, join.channel_id);

    // Initiate SRTP key exchange on the first channel join per connection.
    // Subsequent joins skip this — only the periodic rotation task re-keys.
    if !conn.srtp_initialized {
        let (ke_secret, server_public) = key_exchange::generate_keypair();
        conn.ke_secret = Some(ke_secret);
        let _ = tx.send(Envelope::new("key_exchange_init", &KeyExchangeInitPayload {
            public_key: base64_encode(&server_public),
        }).to_json());
    }

    info!("User '{}' joined channel '{}'", conn.username.as_deref().unwrap_or("?"), channel.name);
}

async fn handle_leave_channel(
    payload: &serde_json::Value,
    state: &AppState,
    conn: &ConnState,
) {
    let leave: LeaveChannelPayload = match serde_json::from_value(payload.clone()) {
        Ok(l) => l,
        Err(_) => return,
    };

    let user_id = match conn.user_id {
        Some(id) => id,
        None => return,
    };
    process_leave_channel(state, user_id, leave.channel_id);
}

fn process_leave_channel(state: &AppState, user_id: i32, channel_id: i32) {
    // Get org_id before removing channel membership
    let org_id = state.connection_manager.get(user_id).map(|h| h.org_id);

    state.channel_manager.leave(channel_id, user_id);
    state.connection_manager.remove_channel(user_id, channel_id);

    // Broadcast user_left
    let left_msg = Envelope::new("user_left", &UserLeftPayload {
        channel_id,
        user_id,
    }).to_json();

    state.connection_manager.broadcast_to_channel(channel_id, &left_msg, None);

    // Broadcast updated user count to all org members
    if let Some(org_id) = org_id {
        broadcast_user_count(state, org_id, channel_id);
    }
}

async fn handle_set_muted(
    payload: &serde_json::Value,
    state: &AppState,
    conn: &ConnState,
) {
    let muted: SetMutedPayload = match serde_json::from_value(payload.clone()) {
        Ok(m) => m,
        Err(_) => return,
    };

    let user_id = match conn.user_id {
        Some(id) => id,
        None => return,
    };
    let presence = state.presence.set_muted(user_id, muted.muted);
    broadcast_state_change(state, user_id, &presence);
}

async fn handle_set_deafened(
    payload: &serde_json::Value,
    state: &AppState,
    conn: &ConnState,
) {
    let deafened: SetDeafenedPayload = match serde_json::from_value(payload.clone()) {
        Ok(d) => d,
        Err(_) => return,
    };

    let user_id = match conn.user_id {
        Some(id) => id,
        None => return,
    };
    let presence = state.presence.set_deafened(user_id, deafened.deafened);
    broadcast_state_change(state, user_id, &presence);
}

fn broadcast_state_change(
    state: &AppState,
    user_id: i32,
    presence: &crate::state::presence::UserPresence,
) {
    // Get all channels this user is in
    if let Some(handle) = state.connection_manager.get(user_id) {
        for channel_id in &handle.channels {
            let msg = Envelope::new("user_state_changed", &UserStateChangedPayload {
                channel_id: *channel_id,
                user_id,
                muted: presence.muted,
                deafened: presence.deafened,
                talking: presence.talking,
            }).to_json();

            state.connection_manager.broadcast_to_channel(*channel_id, &msg, None);
        }
    }
}

/// Broadcast a user_count_changed message to all members of an organization.
fn broadcast_user_count(state: &AppState, org_id: i32, channel_id: i32) {
    let count = state.channel_manager.member_count(channel_id) as i32;
    let msg = Envelope::new("user_count_changed", &UserCountChangedPayload {
        channel_id,
        user_count: count,
    }).to_json();
    state.connection_manager.broadcast_to_org(org_id, &msg);
}

fn build_channel_roster(state: &AppState, channel_id: i32) -> Vec<UserInfo> {
    let members = state.channel_manager.members(channel_id);
    members
        .iter()
        .filter_map(|&uid| {
            state.connection_manager.get(uid).map(|handle| {
                let p = state.presence.get(uid);
                UserInfo {
                    id: uid,
                    username: handle.username.clone(),
                    display_name: handle.display_name.clone(),
                    muted: p.muted,
                    deafened: p.deafened,
                    talking: p.talking,
                }
            })
        })
        .collect()
}

fn get_online_users_for_org(_state: &AppState, _org_id: i32) -> Vec<UserInfo> {
    // Online users are visible per-channel when joining.
    // Full org-wide user list will be populated via admin API.
    Vec::new()
}

async fn handle_key_exchange_response(
    payload: &serde_json::Value,
    state: &AppState,
    tx: &mpsc::UnboundedSender<String>,
    conn: &mut ConnState,
) {
    let ke_resp: KeyExchangeResponsePayload = match serde_json::from_value(payload.clone()) {
        Ok(r) => r,
        Err(_) => {
            let _ = tx.send(make_error("invalid_payload", "Invalid key exchange response"));
            return;
        }
    };

    let user_id = match conn.user_id {
        Some(id) => id,
        None => return,
    };

    // Take the server's ephemeral secret — check conn first, then rotation store
    let server_secret = match conn.ke_secret.take() {
        Some(s) => s,
        None => match crate::crypto::key_rotation::take_rotation_secret(user_id) {
            Some(s) => s,
            None => {
                let _ = tx.send(make_error("key_exchange_error", "No pending key exchange"));
                return;
            }
        },
    };

    // Decode client's public key
    let client_public_bytes = match base64_decode(&ke_resp.public_key) {
        Some(bytes) if bytes.len() == 32 => {
            let mut arr = [0u8; 32];
            arr.copy_from_slice(&bytes);
            arr
        }
        _ => {
            let _ = tx.send(make_error("key_exchange_error", "Invalid public key"));
            return;
        }
    };

    // Compute shared secret and derive keys
    let shared_secret = key_exchange::compute_shared_secret(server_secret, &client_public_bytes);
    let derived_keys = key_exchange::derive_keys(shared_secret.as_bytes());

    // Create SRTP session
    let srtp_session = SrtpSession::new(&derived_keys);
    state.srtp_manager.set_session(user_id, srtp_session);

    conn.srtp_initialized = true;
    info!("SRTP key exchange complete for user {}", user_id);

    // Send key_exchange_complete
    let _ = tx.send(Envelope::new("key_exchange_complete", &KeyExchangeCompletePayload {}).to_json());
}

fn cleanup_connection(user_id: i32, state: &AppState) {
    // Get org_id before removing
    let org_id = state.connection_manager.get(user_id).map(|h| h.org_id);

    // Remove from all channels and broadcast departures
    let left_channels = state.channel_manager.leave_all(user_id);
    for channel_id in &left_channels {
        let left_msg = Envelope::new("user_left", &UserLeftPayload {
            channel_id: *channel_id,
            user_id,
        }).to_json();
        state.connection_manager.broadcast_to_channel(*channel_id, &left_msg, None);
    }

    // Remove connection, presence, and SRTP session
    if let Some(handle) = state.connection_manager.remove(user_id) {
        info!("User '{}' disconnected", handle.username);
    }
    state.presence.remove(user_id);
    state.srtp_manager.remove(user_id);

    // Broadcast updated user counts for all channels the user was in
    if let Some(org_id) = org_id {
        for channel_id in &left_channels {
            broadcast_user_count(state, org_id, *channel_id);
        }
    }
}

fn base64_encode(data: &[u8]) -> String {
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.encode(data)
}

fn base64_decode(data: &str) -> Option<Vec<u8>> {
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.decode(data).ok()
}
