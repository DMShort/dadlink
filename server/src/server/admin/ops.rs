//! Ops API: op templates and live ops. Requires Admin or the OpLeader permission.
//!
//!   GET    /ops/roles                 roles in the org (for building templates)
//!   GET    /ops/templates             list templates (with nets + role access)
//!   POST   /ops/templates             create
//!   PUT    /ops/templates/:id         replace name/description/nets
//!   DELETE /ops/templates/:id
//!   GET    /ops                       active ops
//!   POST   /ops                       start an op from a template {template_id, name}
//!   DELETE /ops/:id                   end an op (removes its channels)

use std::collections::HashSet;

use axum::{
    extract::{Path, State},
    http::StatusCode,
    response::{IntoResponse, Json, Response},
};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::db;
use crate::db::ops::{NetAccess, NetSpec, Op};
use crate::server::messages::{self, ChannelInfo, Envelope};
use crate::server::AppState;

use super::middleware::OpLeaderAuth;

const MAX_NAME_LEN: usize = 64;
const MAX_NETS: usize = 24;

fn err(status: StatusCode, msg: impl Into<String>) -> Response {
    (status, Json(json!({"error": msg.into()}))).into_response()
}

fn db_err(e: anyhow::Error) -> Response {
    let msg = e.to_string();
    if msg.contains("duplicate") || msg.contains("unique") {
        err(StatusCode::CONFLICT, "A template, op or channel with that name already exists")
    } else {
        err(StatusCode::INTERNAL_SERVER_ERROR, format!("Database error: {}", msg))
    }
}

#[derive(Deserialize)]
pub struct TemplateRequest {
    pub name: String,
    pub description: Option<String>,
    #[serde(default)]
    pub nets: Vec<NetSpec>,
}

#[derive(Deserialize)]
pub struct StartOpRequest {
    pub template_id: i32,
    pub name: Option<String>,
}

/// Validate and normalise a template request. Returns a user-facing error on failure.
async fn validate(state: &AppState, org_id: i32, req: &mut TemplateRequest) -> Result<(), Response> {
    req.name = req.name.trim().to_string();
    if req.name.is_empty() || req.name.chars().count() > MAX_NAME_LEN {
        return Err(err(StatusCode::BAD_REQUEST, "Template name must be 1–64 characters"));
    }
    if req.nets.is_empty() {
        return Err(err(StatusCode::BAD_REQUEST, "A template needs at least one net"));
    }
    if req.nets.len() > MAX_NETS {
        return Err(err(StatusCode::BAD_REQUEST, format!("At most {} nets per template", MAX_NETS)));
    }

    let mut names = HashSet::new();
    let mut role_ids = HashSet::new();
    for net in req.nets.iter_mut() {
        net.name = net.name.trim().to_string();
        if net.name.is_empty() || net.name.chars().count() > MAX_NAME_LEN {
            return Err(err(StatusCode::BAD_REQUEST, "Net names must be 1–64 characters"));
        }
        if !names.insert(net.name.to_lowercase()) {
            return Err(err(StatusCode::BAD_REQUEST, format!("Duplicate net name '{}'", net.name)));
        }
        if !(1..=10).contains(&net.priority) {
            return Err(err(StatusCode::BAD_REQUEST, format!("Net '{}': priority must be 1–10", net.name)));
        }
        if !(-1.0..=1.0).contains(&net.pan) || net.pan.is_nan() {
            return Err(err(StatusCode::BAD_REQUEST, format!("Net '{}': pan must be -1..1", net.name)));
        }
        // One entry per role; if listed twice keep the higher access
        net.roles.sort_by_key(|r| (r.role_id, std::cmp::Reverse(r.access)));
        net.roles.dedup_by_key(|r| r.role_id);
        role_ids.extend(net.roles.iter().map(|r| r.role_id));
    }

    if !role_ids.is_empty() {
        let ids: Vec<i32> = role_ids.into_iter().collect();
        match db::ops::count_org_roles(&state.pool, org_id, &ids).await {
            Ok(n) if n as usize == ids.len() => {}
            Ok(_) => return Err(err(StatusCode::BAD_REQUEST, "Unknown role in net assignments")),
            Err(e) => return Err(db_err(e)),
        }
    }
    Ok(())
}

// ─── Roles ───

pub async fn list_roles(State(state): State<AppState>, OpLeaderAuth(auth): OpLeaderAuth) -> Response {
    let rows: Result<Vec<(i32, String, Option<String>, Option<i32>)>, _> = sqlx::query_as(
        "SELECT id, name, color, priority FROM roles WHERE org_id = $1 ORDER BY priority DESC NULLS LAST, name",
    )
    .bind(auth.org_id)
    .fetch_all(&state.pool)
    .await;
    match rows {
        Ok(rows) => {
            let roles: Vec<_> = rows
                .into_iter()
                .map(|(id, name, color, priority)| json!({"id": id, "name": name, "color": color, "priority": priority}))
                .collect();
            Json(json!({"roles": roles})).into_response()
        }
        Err(e) => db_err(e.into()),
    }
}

// ─── Templates ───

pub async fn list_templates(State(state): State<AppState>, OpLeaderAuth(auth): OpLeaderAuth) -> Response {
    match db::ops::list_templates(&state.pool, auth.org_id).await {
        Ok(t) => Json(json!({"templates": t})).into_response(),
        Err(e) => db_err(e),
    }
}

pub async fn create_template(
    State(state): State<AppState>,
    OpLeaderAuth(auth): OpLeaderAuth,
    Json(mut body): Json<TemplateRequest>,
) -> Response {
    if let Err(r) = validate(&state, auth.org_id, &mut body).await {
        return r;
    }
    match db::ops::create_template(
        &state.pool, auth.org_id, auth.user_id, &body.name, body.description.as_deref(), &body.nets,
    )
    .await
    {
        Ok(id) => {
            let _ = db::audit::log_action(
                &state.pool, auth.org_id, auth.user_id, "create_op_template",
                Some("op_template"), Some(id), Some(json!({"name": body.name})), None,
            )
            .await;
            match db::ops::get_template(&state.pool, auth.org_id, id).await {
                Ok(Some(t)) => (StatusCode::CREATED, Json(json!({"template": t}))).into_response(),
                Ok(None) => err(StatusCode::INTERNAL_SERVER_ERROR, "Template vanished after create"),
                Err(e) => db_err(e),
            }
        }
        Err(e) => db_err(e),
    }
}

pub async fn update_template(
    State(state): State<AppState>,
    OpLeaderAuth(auth): OpLeaderAuth,
    Path(template_id): Path<i32>,
    Json(mut body): Json<TemplateRequest>,
) -> Response {
    match db::ops::get_template(&state.pool, auth.org_id, template_id).await {
        Ok(Some(_)) => {}
        Ok(None) => return err(StatusCode::NOT_FOUND, "Template not found"),
        Err(e) => return db_err(e),
    }
    if let Err(r) = validate(&state, auth.org_id, &mut body).await {
        return r;
    }
    if let Err(e) = db::ops::update_template(
        &state.pool, template_id, &body.name, body.description.as_deref(), &body.nets,
    )
    .await
    {
        return db_err(e);
    }
    let _ = db::audit::log_action(
        &state.pool, auth.org_id, auth.user_id, "update_op_template",
        Some("op_template"), Some(template_id), Some(json!({"name": body.name})), None,
    )
    .await;
    match db::ops::get_template(&state.pool, auth.org_id, template_id).await {
        Ok(Some(t)) => Json(json!({"template": t})).into_response(),
        Ok(None) => err(StatusCode::NOT_FOUND, "Template not found"),
        Err(e) => db_err(e),
    }
}

pub async fn delete_template(
    State(state): State<AppState>,
    OpLeaderAuth(auth): OpLeaderAuth,
    Path(template_id): Path<i32>,
) -> Response {
    match db::ops::get_template(&state.pool, auth.org_id, template_id).await {
        Ok(Some(_)) => {}
        Ok(None) => return err(StatusCode::NOT_FOUND, "Template not found"),
        Err(e) => return db_err(e),
    }
    if let Err(e) = db::ops::delete_template(&state.pool, template_id).await {
        return db_err(e);
    }
    let _ = db::audit::log_action(
        &state.pool, auth.org_id, auth.user_id, "delete_op_template",
        Some("op_template"), Some(template_id), None, None,
    )
    .await;
    Json(json!({"status": "ok"})).into_response()
}

// ─── Live ops ───

pub async fn list_ops(State(state): State<AppState>, OpLeaderAuth(auth): OpLeaderAuth) -> Response {
    match db::ops::list_active_ops(&state.pool, auth.org_id).await {
        Ok(ops) => Json(json!({"ops": ops})).into_response(),
        Err(e) => db_err(e),
    }
}

pub async fn start_op(
    State(state): State<AppState>,
    OpLeaderAuth(auth): OpLeaderAuth,
    Json(body): Json<StartOpRequest>,
) -> Response {
    let template = match db::ops::get_template(&state.pool, auth.org_id, body.template_id).await {
        Ok(Some(t)) => t,
        Ok(None) => return err(StatusCode::NOT_FOUND, "Template not found"),
        Err(e) => return db_err(e),
    };
    if template.nets.is_empty() {
        return err(StatusCode::BAD_REQUEST, "Template has no nets");
    }
    let name = body
        .name
        .map(|n| n.trim().to_string())
        .filter(|n| !n.is_empty())
        .unwrap_or_else(|| template.name.clone());
    if name.chars().count() > MAX_NAME_LEN {
        return err(StatusCode::BAD_REQUEST, "Op name must be 1–64 characters");
    }

    let started = match db::ops::start_op(&state.pool, auth.org_id, auth.user_id, &name, &template).await {
        Ok(s) => s,
        Err(e) => return db_err(e),
    };

    // Make the new channels live: limits + channel_updated to everyone in the org
    for ch in &started.channels {
        state.channel_manager.set_limit(ch.id, ch.max_users.unwrap_or(0));
        let info = ChannelInfo {
            id: ch.id,
            name: ch.name.clone(),
            description: ch.description.clone(),
            parent_id: ch.parent_id,
            position: ch.position.unwrap_or(0),
            max_users: ch.max_users.unwrap_or(0),
            has_password: false,
            user_count: 0,
        };
        let env = Envelope::new("channel_updated", &messages::ChannelUpdatedPayload { channel: info });
        state.connection_manager.broadcast_to_org(auth.org_id, &env.to_json());
    }

    let _ = db::audit::log_action(
        &state.pool, auth.org_id, auth.user_id, "start_op",
        Some("op"), Some(started.op_id), Some(json!({"name": name, "template_id": template.id})), None,
    )
    .await;

    let op = match db::ops::list_active_ops(&state.pool, auth.org_id).await {
        Ok(ops) => ops.into_iter().find(|o| o.id == started.op_id),
        Err(e) => return db_err(e),
    };
    let Some(op) = op else {
        return err(StatusCode::INTERNAL_SERVER_ERROR, "Op vanished after start");
    };

    // Tell every online member their own nets and access
    for user_id in state.connection_manager.users_in_org(auth.org_id) {
        send_op_started(&state, user_id, &op).await;
    }

    (StatusCode::CREATED, Json(json!({"op": op}))).into_response()
}

pub async fn end_op(
    State(state): State<AppState>,
    OpLeaderAuth(auth): OpLeaderAuth,
    Path(op_id): Path<i32>,
) -> Response {
    let op = match db::ops::list_active_ops(&state.pool, auth.org_id).await {
        Ok(ops) => ops.into_iter().find(|o| o.id == op_id),
        Err(e) => return db_err(e),
    };
    let Some(op) = op else {
        return err(StatusCode::NOT_FOUND, "Op not found or already ended");
    };

    let deleted = match db::ops::end_op(&state.pool, &op).await {
        Ok(d) => d,
        Err(e) => return db_err(e),
    };

    // Remove everyone from the op's channels and tell the whole org they're gone
    for channel_id in &deleted {
        for user_id in state.channel_manager.members(*channel_id) {
            state.connection_manager.remove_channel(user_id, *channel_id);
        }
        state.channel_manager.remove_channel(*channel_id);
        let env = Envelope::new("channel_deleted", &messages::ChannelDeletedPayload { channel_id: *channel_id });
        state.connection_manager.broadcast_to_org(auth.org_id, &env.to_json());
    }
    let env = Envelope::new("op_ended", &OpEndedPayload { op_id: op.id, name: op.name.clone() });
    state.connection_manager.broadcast_to_org(auth.org_id, &env.to_json());

    let _ = db::audit::log_action(
        &state.pool, auth.org_id, auth.user_id, "end_op",
        Some("op"), Some(op.id), Some(json!({"name": op.name})), None,
    )
    .await;

    Json(json!({"status": "ok", "deleted_channels": deleted})).into_response()
}

// ─── Personalised op messages (WebSocket) ───

#[derive(Debug, Serialize)]
pub struct OpNetForUser {
    pub channel_id: i32,
    pub name: String,
    pub priority: i32,
    pub pan: f32,
    pub radio: bool,
    /// What this user can do on the net: "talk", "listen", or null (no access).
    pub access: Option<NetAccess>,
    /// True if one of the user's roles is assigned to this net: the client auto-joins these.
    pub assigned: bool,
}

#[derive(Debug, Serialize)]
pub struct OpForUser {
    pub id: i32,
    pub name: String,
    pub parent_channel_id: Option<i32>,
    pub nets: Vec<OpNetForUser>,
}

#[derive(Debug, Serialize)]
pub struct OpStartedPayload {
    pub op: OpForUser,
}

#[derive(Debug, Serialize)]
pub struct OpEndedPayload {
    pub op_id: i32,
    pub name: String,
}

/// Work out one user's view of an op from their roles.
pub fn op_for_user(op: &Op, role_ids: &[i32], is_admin: bool) -> OpForUser {
    let nets = op
        .nets
        .iter()
        .map(|n| {
            let from_roles = n
                .roles
                .iter()
                .filter(|r| role_ids.contains(&r.role_id))
                .map(|r| r.access)
                .max();
            let assigned = from_roles.is_some();
            let access = if n.roles.is_empty() || is_admin {
                Some(NetAccess::Talk) // open net, or admin bypass
            } else {
                from_roles
            };
            OpNetForUser {
                channel_id: n.channel_id,
                name: n.name.clone(),
                priority: n.priority,
                pan: n.pan,
                radio: n.radio,
                access,
                assigned,
            }
        })
        .collect();
    OpForUser { id: op.id, name: op.name.clone(), parent_channel_id: op.parent_channel_id, nets }
}

/// Send `op_started` for one op to one user, personalised to their roles.
pub async fn send_op_started(state: &AppState, user_id: i32, op: &Op) {
    let role_ids = db::ops::user_role_ids(&state.pool, user_id).await.unwrap_or_default();
    let is_admin = db::ops::user_is_admin(&state.pool, user_id).await.unwrap_or(false);
    let env = Envelope::new("op_started", &OpStartedPayload { op: op_for_user(op, &role_ids, is_admin) });
    state.connection_manager.send_to(user_id, &env.to_json());
}

/// After login: tell the user about every op already running in their org.
pub async fn send_active_ops(state: &AppState, user_id: i32, org_id: i32) {
    let Ok(ops) = db::ops::list_active_ops(&state.pool, org_id).await else { return };
    if ops.is_empty() {
        return;
    }
    let role_ids = db::ops::user_role_ids(&state.pool, user_id).await.unwrap_or_default();
    let is_admin = db::ops::user_is_admin(&state.pool, user_id).await.unwrap_or(false);
    for op in &ops {
        let env = Envelope::new("op_started", &OpStartedPayload { op: op_for_user(op, &role_ids, is_admin) });
        state.connection_manager.send_to(user_id, &env.to_json());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::ops::{OpNet, RoleAccess};

    fn op() -> Op {
        let net = |id, name: &str, roles: Vec<(i32, NetAccess)>| OpNet {
            channel_id: id,
            name: name.into(),
            position: 0,
            priority: 5,
            pan: 0.0,
            radio: false,
            roles: roles.into_iter().map(|(role_id, access)| RoleAccess { role_id, access }).collect(),
        };
        Op {
            id: 1, org_id: 1, template_id: None, name: "Nightfall".into(),
            parent_channel_id: Some(10), started_by: None, started_at: None,
            nets: vec![
                net(11, "Command", vec![(1, NetAccess::Talk), (2, NetAccess::Listen)]),
                net(12, "Fleet", vec![(2, NetAccess::Talk)]),
                net(13, "Open", vec![]),
            ],
        }
    }

    #[test]
    fn pilot_gets_listen_on_command_and_talk_on_fleet() {
        let v = op_for_user(&op(), &[2], false);
        assert_eq!(v.nets[0].access, Some(NetAccess::Listen));
        assert!(v.nets[0].assigned);
        assert_eq!(v.nets[1].access, Some(NetAccess::Talk));
        assert!(v.nets[1].assigned);
        // Open net: anyone may talk, but it's not auto-joined
        assert_eq!(v.nets[2].access, Some(NetAccess::Talk));
        assert!(!v.nets[2].assigned);
    }

    #[test]
    fn highest_access_wins_across_roles() {
        let v = op_for_user(&op(), &[1, 2], false);
        assert_eq!(v.nets[0].access, Some(NetAccess::Talk));
    }

    #[test]
    fn unassigned_user_has_no_access_to_restricted_nets() {
        let v = op_for_user(&op(), &[99], false);
        assert_eq!(v.nets[0].access, None);
        assert_eq!(v.nets[1].access, None);
        assert!(!v.nets[0].assigned);
    }

    #[test]
    fn admin_can_talk_everywhere_but_only_auto_joins_assigned() {
        let v = op_for_user(&op(), &[], true);
        assert!(v.nets.iter().all(|n| n.access == Some(NetAccess::Talk)));
        assert!(v.nets.iter().all(|n| !n.assigned));
    }
}
