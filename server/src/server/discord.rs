//! "Login with Discord" (OAuth2 authorization-code flow) with a desktop-friendly handoff.
//!
//! 1. The client makes a random `verifier`, and opens the browser at
//!    `/auth/discord/start?org=<tag>&challenge=<sha256-hex(verifier)>`.
//! 2. We redirect to Discord. Discord sends the browser back to `/auth/discord/callback`.
//! 3. We exchange the code, read the Discord user (and guild membership/roles if a guild is
//!    configured), find or create the DadLink user, sync Discord roles, and park a
//!    single-use ticket under the challenge.
//! 4. The client polls `/auth/discord/poll?verifier=<verifier>`; we hash it, find the ticket and
//!    hand it over once. The client then authenticates the WebSocket with
//!    `{method: "discord", token: <ticket>}`.
//!
//! The browser URL only ever contains the challenge, so a copied link or browser history
//! can't be used to collect the ticket. Tickets expire after 2 minutes and work once.

use std::collections::HashSet;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use axum::{
    extract::{Query, State},
    http::StatusCode,
    response::{Html, IntoResponse, Json, Redirect, Response},
    routing::get,
    Router,
};
use dashmap::DashMap;
use rand::RngCore;
use serde::Deserialize;
use serde_json::json;
use sha2::{Digest, Sha256};
use tracing::{info, warn};

use crate::auth::password;
use crate::db;
use crate::server::AppState;

const STATE_TTL: Duration = Duration::from_secs(10 * 60);
const TICKET_TTL: Duration = Duration::from_secs(2 * 60);
const MAX_PENDING: usize = 2000;

struct PendingLogin {
    org_id: i32,
    challenge: String,
    created: Instant,
}

enum Outcome {
    Ticket(String),
    Failed(String),
}

struct Parked {
    outcome: Outcome,
    created: Instant,
}

struct TicketInfo {
    user_id: i32,
    created: Instant,
}

#[derive(Default)]
struct Store {
    /// OAuth `state` -> pending login (browser is at Discord)
    states: DashMap<String, PendingLogin>,
    /// challenge -> outcome, waiting for the client to poll
    results: DashMap<String, Parked>,
    /// ticket -> user, waiting for the WebSocket authenticate
    tickets: DashMap<String, TicketInfo>,
}

fn store() -> &'static Store {
    static STORE: OnceLock<Store> = OnceLock::new();
    STORE.get_or_init(Store::default)
}

fn random_hex(bytes: usize) -> String {
    let mut buf = vec![0u8; bytes];
    rand::thread_rng().fill_bytes(&mut buf);
    buf.iter().map(|b| format!("{:02x}", b)).collect()
}

fn sha256_hex(s: &str) -> String {
    Sha256::digest(s.as_bytes()).iter().map(|b| format!("{:02x}", b)).collect()
}

fn is_hex(s: &str, len: usize) -> bool {
    s.len() == len && s.bytes().all(|b| b.is_ascii_hexdigit())
}

fn prune() {
    let s = store();
    s.states.retain(|_, v| v.created.elapsed() < STATE_TTL);
    s.results.retain(|_, v| v.created.elapsed() < STATE_TTL);
    s.tickets.retain(|_, v| v.created.elapsed() < TICKET_TTL);
}

pub fn routes() -> Router<AppState> {
    Router::new()
        .route("/auth/config", get(auth_config))
        .route("/auth/discord/start", get(start))
        .route("/auth/discord/callback", get(callback))
        .route("/auth/discord/poll", get(poll))
}

/// Which login methods this server offers (the client uses it to show the Discord button).
async fn auth_config(State(state): State<AppState>) -> Json<serde_json::Value> {
    Json(json!({ "discord": state.config.discord.enabled() }))
}

#[derive(Deserialize)]
pub struct StartQuery {
    org: String,
    challenge: String,
}

async fn start(State(state): State<AppState>, Query(q): Query<StartQuery>) -> Response {
    let cfg = &state.config.discord;
    if !cfg.enabled() {
        return page(StatusCode::NOT_FOUND, "Discord login is not enabled on this server.");
    }
    let challenge = q.challenge.to_lowercase();
    if !is_hex(&challenge, 64) {
        return page(StatusCode::BAD_REQUEST, "Invalid login request.");
    }
    let org_id: Option<(i32,)> = sqlx::query_as("SELECT id FROM organizations WHERE tag = $1")
        .bind(&q.org)
        .fetch_optional(&state.pool)
        .await
        .unwrap_or(None);
    let Some((org_id,)) = org_id else {
        return page(StatusCode::NOT_FOUND, "Unknown organisation.");
    };

    prune();
    if store().states.len() >= MAX_PENDING {
        return page(StatusCode::SERVICE_UNAVAILABLE, "Too many logins in progress. Try again in a minute.");
    }
    let oauth_state = random_hex(16);
    store().states.insert(oauth_state.clone(), PendingLogin { org_id, challenge, created: Instant::now() });

    let scope = if cfg.guild_id.is_some() { "identify guilds.members.read" } else { "identify" };
    let url = reqwest::Url::parse_with_params(
        &cfg.authorize_url,
        &[
            ("response_type", "code"),
            ("client_id", cfg.client_id.as_deref().unwrap_or("")),
            ("scope", scope),
            ("redirect_uri", cfg.redirect_uri.as_deref().unwrap_or("")),
            ("state", oauth_state.as_str()),
            ("prompt", "none"),
        ],
    );
    match url {
        Ok(u) => Redirect::to(u.as_str()).into_response(),
        Err(_) => page(StatusCode::INTERNAL_SERVER_ERROR, "Discord login is misconfigured (authorize URL)."),
    }
}

#[derive(Deserialize)]
pub struct CallbackQuery {
    code: Option<String>,
    state: Option<String>,
    error: Option<String>,
}

async fn callback(State(state): State<AppState>, Query(q): Query<CallbackQuery>) -> Response {
    let Some(pending) = q.state.as_deref().and_then(|s| store().states.remove(s)).map(|(_, v)| v) else {
        return page(StatusCode::BAD_REQUEST, "This login link has expired. Start again from DadLink.");
    };
    if pending.created.elapsed() > STATE_TTL {
        return page(StatusCode::BAD_REQUEST, "This login link has expired. Start again from DadLink.");
    }

    let outcome = match (q.error.as_deref(), q.code.as_deref()) {
        (Some(e), _) => Err(format!("Discord login was cancelled ({e}).")),
        (None, Some(code)) => complete_login(&state, pending.org_id, code).await,
        (None, None) => Err("Discord didn't return a login code.".to_string()),
    };

    let (status, msg) = match &outcome {
        Ok((_, name)) => (StatusCode::OK, format!("Signed in as {name}. You can close this tab and return to DadLink.")),
        Err(e) => (StatusCode::FORBIDDEN, e.clone()),
    };
    let parked = match outcome {
        Ok((user_id, _)) => {
            let ticket = random_hex(32);
            store().tickets.insert(ticket.clone(), TicketInfo { user_id, created: Instant::now() });
            Outcome::Ticket(ticket)
        }
        Err(e) => Outcome::Failed(e),
    };
    store().results.insert(pending.challenge, Parked { outcome: parked, created: Instant::now() });
    page(status, &msg)
}

#[derive(Deserialize)]
pub struct PollQuery {
    verifier: String,
}

async fn poll(Query(q): Query<PollQuery>) -> Response {
    if !is_hex(&q.verifier, 64) {
        return (StatusCode::BAD_REQUEST, Json(json!({"status": "error", "reason": "bad verifier"}))).into_response();
    }
    let challenge = sha256_hex(&q.verifier.to_lowercase());
    match store().results.remove(&challenge) {
        None => Json(json!({"status": "pending"})).into_response(),
        Some((_, p)) if p.created.elapsed() > TICKET_TTL => {
            Json(json!({"status": "error", "reason": "Login expired. Please try again."})).into_response()
        }
        Some((_, Parked { outcome: Outcome::Ticket(t), .. })) => Json(json!({"status": "ok", "ticket": t})).into_response(),
        Some((_, Parked { outcome: Outcome::Failed(e), .. })) => Json(json!({"status": "error", "reason": e})).into_response(),
    }
}

/// Redeem a single-use ticket from the WebSocket `authenticate {method: "discord"}`.
pub fn redeem_ticket(ticket: &str) -> Option<i32> {
    let (_, info) = store().tickets.remove(ticket)?;
    (info.created.elapsed() <= TICKET_TTL).then_some(info.user_id)
}

#[derive(Deserialize)]
struct TokenResponse {
    access_token: String,
}

#[derive(Deserialize)]
struct DiscordUser {
    id: String,
    username: String,
    global_name: Option<String>,
}

#[derive(Deserialize)]
struct GuildMember {
    nick: Option<String>,
    #[serde(default)]
    roles: Vec<String>,
}

fn http() -> &'static reqwest::Client {
    static CLIENT: OnceLock<reqwest::Client> = OnceLock::new();
    CLIENT.get_or_init(|| {
        reqwest::Client::builder()
            .timeout(Duration::from_secs(10))
            .user_agent("DadLink (https://github.com/DMShort/dadlink)")
            .build()
            .expect("reqwest client")
    })
}

/// Exchange the code, check guild membership, find/create the user and sync roles.
/// Returns (user_id, display name).
async fn complete_login(state: &AppState, org_id: i32, code: &str) -> Result<(i32, String), String> {
    let cfg = &state.config.discord;
    let api = cfg.api_base.trim_end_matches('/');

    let token: TokenResponse = http()
        .post(format!("{api}/oauth2/token"))
        .form(&[
            ("client_id", cfg.client_id.as_deref().unwrap_or("")),
            ("client_secret", cfg.client_secret.as_deref().unwrap_or("")),
            ("grant_type", "authorization_code"),
            ("code", code),
            ("redirect_uri", cfg.redirect_uri.as_deref().unwrap_or("")),
        ])
        .send()
        .await
        .and_then(|r| r.error_for_status())
        .map_err(|e| { warn!("Discord token exchange failed: {e}"); "Discord rejected the login. Please try again.".to_string() })?
        .json()
        .await
        .map_err(|_| "Unexpected reply from Discord.".to_string())?;

    let me: DiscordUser = http()
        .get(format!("{api}/users/@me"))
        .bearer_auth(&token.access_token)
        .send()
        .await
        .and_then(|r| r.error_for_status())
        .map_err(|e| { warn!("Discord /users/@me failed: {e}"); "Could not read your Discord profile.".to_string() })?
        .json()
        .await
        .map_err(|_| "Unexpected reply from Discord.".to_string())?;

    // Guild membership (and roles) if a guild is configured
    let member: Option<GuildMember> = match cfg.guild_id.as_deref().filter(|g| !g.is_empty()) {
        None => None,
        Some(guild) => {
            let resp = http()
                .get(format!("{api}/users/@me/guilds/{guild}/member"))
                .bearer_auth(&token.access_token)
                .send()
                .await
                .map_err(|_| "Could not check your Discord server membership.".to_string())?;
            if resp.status() == reqwest::StatusCode::NOT_FOUND {
                return Err("You need to be a member of the org's Discord server to log in.".into());
            }
            Some(resp.error_for_status()
                .map_err(|_| "Could not check your Discord server membership.".to_string())?
                .json()
                .await
                .map_err(|_| "Unexpected reply from Discord.".to_string())?)
        }
    };

    let display_name = member
        .as_ref()
        .and_then(|m| m.nick.clone())
        .or_else(|| me.global_name.clone())
        .unwrap_or_else(|| me.username.clone());

    // Find the linked account, or create one
    let existing: Option<(i32, bool)> = sqlx::query_as(
        "SELECT id, COALESCE(is_active, TRUE) FROM users WHERE org_id = $1 AND discord_id = $2",
    )
    .bind(org_id)
    .bind(&me.id)
    .fetch_optional(&state.pool)
    .await
    .map_err(|_| "Internal error".to_string())?;

    let user_id = match existing {
        Some((_, false)) => return Err("Your DadLink account is disabled. Ask an admin.".into()),
        Some((id, true)) => {
            // Keep the display name in step with Discord
            let _ = sqlx::query("UPDATE users SET display_name = $2 WHERE id = $1")
                .bind(id)
                .bind(&display_name)
                .execute(&state.pool)
                .await;
            id
        }
        None if !cfg.auto_create_users => {
            return Err("No DadLink account is linked to this Discord account. Ask an admin to link it.".into())
        }
        None => create_discord_user(state, org_id, &me, &display_name).await?,
    };

    if let Some(m) = &member {
        sync_roles(state, org_id, user_id, &m.roles).await;
    }

    info!("Discord login: discord_id={} -> user {} ({})", me.id, user_id, display_name);
    Ok((user_id, display_name))
}

/// Make a unique DadLink username from the Discord username and create the account.
/// The password is random and never shown, so the account can only log in via Discord
/// (until an admin sets a password).
async fn create_discord_user(
    state: &AppState,
    org_id: i32,
    me: &DiscordUser,
    display: &str,
) -> Result<i32, String> {
    let base: String = me
        .username
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '_' || *c == '.')
        .take(28)
        .collect();
    let base = if base.len() < 3 { format!("discord_{}", &me.id[me.id.len().saturating_sub(6)..]) } else { base };

    let hash = password::hash_password(&random_hex(32)).map_err(|_| "Internal error".to_string())?;
    for attempt in 0..20 {
        let username = if attempt == 0 { base.clone() } else { format!("{base}{}", attempt + 1) };
        let res: Result<(i32,), _> = sqlx::query_as(
            "INSERT INTO users (org_id, username, password_hash, display_name, is_admin, discord_id)
             VALUES ($1, $2, $3, $4, FALSE, $5) RETURNING id",
        )
        .bind(org_id)
        .bind(&username)
        .bind(&hash)
        .bind(display)
        .bind(&me.id)
        .fetch_one(&state.pool)
        .await;
        match res {
            Ok((id,)) => {
                let _ = db::audit::log_action(
                    &state.pool, org_id, id, "discord_account_created",
                    Some("user"), Some(id), Some(json!({"username": username, "discord_id": me.id})), None,
                )
                .await;
                return Ok(id);
            }
            Err(e) if e.to_string().contains("duplicate") => {
                // Username taken (or, in a race, the discord id was linked meanwhile)
                let linked: Option<(i32,)> = sqlx::query_as("SELECT id FROM users WHERE org_id = $1 AND discord_id = $2")
                    .bind(org_id)
                    .bind(&me.id)
                    .fetch_optional(&state.pool)
                    .await
                    .unwrap_or(None);
                if let Some((id,)) = linked {
                    return Ok(id);
                }
            }
            Err(e) => {
                warn!("Creating Discord user failed: {e}");
                return Err("Internal error".into());
            }
        }
    }
    Err("Could not pick a free username. Ask an admin to create your account.".into())
}

/// Roles with a discord_role_id follow Discord: granted if the member has that Discord role,
/// removed if not. Roles without a discord_role_id are left alone.
async fn sync_roles(state: &AppState, org_id: i32, user_id: i32, discord_roles: &[String]) {
    let mapped: Vec<(i32, String)> = match sqlx::query_as(
        "SELECT id, discord_role_id FROM roles WHERE org_id = $1 AND discord_role_id IS NOT NULL AND discord_role_id <> ''",
    )
    .bind(org_id)
    .fetch_all(&state.pool)
    .await
    {
        Ok(r) => r,
        Err(e) => { warn!("Role sync query failed: {e}"); return; }
    };
    let have: HashSet<&str> = discord_roles.iter().map(String::as_str).collect();
    for (role_id, discord_role) in mapped {
        if have.contains(discord_role.as_str()) {
            let _ = sqlx::query("INSERT INTO user_roles (user_id, role_id) VALUES ($1, $2) ON CONFLICT DO NOTHING")
                .bind(user_id)
                .bind(role_id)
                .execute(&state.pool)
                .await;
        } else {
            let _ = sqlx::query("DELETE FROM user_roles WHERE user_id = $1 AND role_id = $2")
                .bind(user_id)
                .bind(role_id)
                .execute(&state.pool)
                .await;
        }
    }
}

fn page(status: StatusCode, message: &str) -> Response {
    let esc = message.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
    let ok = status.is_success();
    let html = format!(
        r#"<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DadLink login</title><style>
body{{margin:0;min-height:100vh;display:grid;place-items:center;background:#111418;color:#e8e8e8;font:16px/1.5 system-ui,sans-serif}}
.card{{max-width:420px;padding:28px 32px;border-radius:10px;background:#1b1f26;border:1px solid #2c323c;text-align:center}}
h1{{font-size:20px;margin:0 0 8px;color:{color}}}</style></head>
<body><div class="card"><h1>{title}</h1><p>{esc}</p></div></body></html>"#,
        color = if ok { "#6fdc8c" } else { "#ff7b7b" },
        title = if ok { "You're signed in" } else { "Login problem" },
    );
    (status, Html(html)).into_response()
}
