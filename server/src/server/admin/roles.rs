use axum::{
    extract::{Path, State},
    http::StatusCode,
    response::{IntoResponse, Json},
};
use serde::Deserialize;
use serde_json::json;

use crate::db;
use crate::server::AppState;

use super::middleware::AdminAuth;

#[derive(Deserialize)]
pub struct CreateRoleRequest {
    pub name: String,
    pub permissions: i32,
    pub priority: Option<i32>,
    pub color: Option<String>,
    /// Discord role id to sync from on Discord login ("" or absent = none)
    pub discord_role_id: Option<String>,
}

#[derive(Deserialize)]
pub struct UpdateRoleRequest {
    pub name: Option<String>,
    pub permissions: Option<i32>,
    pub priority: Option<i32>,
    pub color: Option<String>,
    /// Absent = unchanged, "" = clear
    pub discord_role_id: Option<String>,
}

/// Set or clear a role's Discord role id. Discord ids are numeric snowflakes.
fn validate_discord_role(value: &str) -> Result<(), axum::response::Response> {
    let v = value.trim();
    if !v.is_empty() && (v.len() > 32 || !v.bytes().all(|b| b.is_ascii_digit())) {
        return Err((StatusCode::BAD_REQUEST, Json(json!({"error":
            "Discord role ID must be the numeric ID (enable Developer Mode in Discord, right-click the role, Copy Role ID)"}))).into_response());
    }
    Ok(())
}

async fn set_discord_role(pool: &sqlx::PgPool, role_id: i32, value: &str) -> Result<(), String> {
    let v = value.trim();
    sqlx::query("UPDATE roles SET discord_role_id = NULLIF($2, '') WHERE id = $1")
        .bind(role_id)
        .bind(v)
        .execute(pool)
        .await
        .map_err(|e| e.to_string())?;
    Ok(())
}

pub async fn list_roles(
    State(state): State<AppState>,
    auth: AdminAuth,
) -> impl IntoResponse {
    match db::roles::list_by_org(&state.pool, auth.org_id).await {
        Ok(roles) => Json(json!({"roles": roles})).into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(json!({"error": format!("Failed to list roles: {}", e)})),
        )
            .into_response(),
    }
}

pub async fn create_role(
    State(state): State<AppState>,
    auth: AdminAuth,
    Json(body): Json<CreateRoleRequest>,
) -> impl IntoResponse {
    if body.name.is_empty() {
        return (
            StatusCode::BAD_REQUEST,
            Json(json!({"error": "Role name is required"})),
        )
            .into_response();
    }

    if let Some(d) = &body.discord_role_id {
        if let Err(r) = validate_discord_role(d) { return r; }
    }

    match db::roles::create(
        &state.pool,
        auth.org_id,
        &body.name,
        body.permissions,
        body.priority.unwrap_or(0),
        body.color.as_deref(),
    )
    .await
    {
        Ok(mut role) => {
            if let Some(d) = &body.discord_role_id {
                if set_discord_role(&state.pool, role.id, d).await.is_ok() {
                    role.discord_role_id = Some(d.trim().to_string()).filter(|s| !s.is_empty());
                }
            }
            let _ = db::audit::log_action(
                &state.pool,
                auth.org_id,
                auth.user_id,
                "create_role",
                Some("role"),
                Some(role.id),
                Some(json!({"name": role.name, "permissions": role.permissions})),
                None,
            )
            .await;

            (StatusCode::CREATED, Json(json!({"role": role}))).into_response()
        }
        Err(e) => {
            let msg = e.to_string();
            if msg.contains("duplicate") || msg.contains("unique") {
                (
                    StatusCode::CONFLICT,
                    Json(json!({"error": "Role name already exists in this organization"})),
                )
                    .into_response()
            } else {
                (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    Json(json!({"error": format!("Failed to create role: {}", e)})),
                )
                    .into_response()
            }
        }
    }
}

pub async fn update_role(
    State(state): State<AppState>,
    auth: AdminAuth,
    Path(role_id): Path<i32>,
    Json(body): Json<UpdateRoleRequest>,
) -> impl IntoResponse {
    // Verify role belongs to same org
    let existing = match db::roles::get_by_id(&state.pool, role_id).await {
        Ok(Some(r)) if r.org_id == auth.org_id => r,
        Ok(Some(_)) => {
            return (
                StatusCode::FORBIDDEN,
                Json(json!({"error": "Role belongs to a different organization"})),
            )
                .into_response();
        }
        Ok(None) => {
            return (
                StatusCode::NOT_FOUND,
                Json(json!({"error": "Role not found"})),
            )
                .into_response();
        }
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(json!({"error": format!("Database error: {}", e)})),
            )
                .into_response();
        }
    };

    let name = body.name.as_deref().unwrap_or(&existing.name);
    let permissions = body.permissions.unwrap_or(existing.permissions);
    let priority = body.priority.unwrap_or(existing.priority.unwrap_or(0));
    let color = body.color.as_deref().or(existing.color.as_deref());

    if let Some(d) = &body.discord_role_id {
        if let Err(r) = validate_discord_role(d) { return r; }
    }

    match db::roles::update(&state.pool, role_id, name, permissions, priority, color).await {
        Ok(mut role) => {
            if let Some(d) = &body.discord_role_id {
                if set_discord_role(&state.pool, role_id, d).await.is_ok() {
                    role.discord_role_id = Some(d.trim().to_string()).filter(|s| !s.is_empty());
                }
            }
            let _ = db::audit::log_action(
                &state.pool,
                auth.org_id,
                auth.user_id,
                "update_role",
                Some("role"),
                Some(role_id),
                Some(json!({"name": role.name, "permissions": role.permissions})),
                None,
            )
            .await;

            Json(json!({"role": role})).into_response()
        }
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(json!({"error": format!("Failed to update role: {}", e)})),
        )
            .into_response(),
    }
}

pub async fn delete_role(
    State(state): State<AppState>,
    auth: AdminAuth,
    Path(role_id): Path<i32>,
) -> impl IntoResponse {
    // Verify role belongs to same org
    match db::roles::get_by_id(&state.pool, role_id).await {
        Ok(Some(r)) if r.org_id == auth.org_id => {}
        Ok(Some(_)) => {
            return (
                StatusCode::FORBIDDEN,
                Json(json!({"error": "Role belongs to a different organization"})),
            )
                .into_response();
        }
        Ok(None) => {
            return (
                StatusCode::NOT_FOUND,
                Json(json!({"error": "Role not found"})),
            )
                .into_response();
        }
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(json!({"error": format!("Database error: {}", e)})),
            )
                .into_response();
        }
    }

    match db::roles::delete(&state.pool, role_id).await {
        Ok(_) => {
            let _ = db::audit::log_action(
                &state.pool,
                auth.org_id,
                auth.user_id,
                "delete_role",
                Some("role"),
                Some(role_id),
                None,
                None,
            )
            .await;

            Json(json!({"status": "ok"})).into_response()
        }
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(json!({"error": format!("Failed to delete role: {}", e)})),
        )
            .into_response(),
    }
}

pub async fn assign_role(
    State(state): State<AppState>,
    auth: AdminAuth,
    Path((role_id, user_id)): Path<(i32, i32)>,
) -> impl IntoResponse {
    // Verify role belongs to same org
    match db::roles::get_by_id(&state.pool, role_id).await {
        Ok(Some(r)) if r.org_id == auth.org_id => {}
        Ok(Some(_)) | Ok(None) => {
            return (
                StatusCode::NOT_FOUND,
                Json(json!({"error": "Role not found"})),
            )
                .into_response();
        }
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(json!({"error": format!("Database error: {}", e)})),
            )
                .into_response();
        }
    }

    // Verify user belongs to same org
    match db::users::find_by_id(&state.pool, user_id).await {
        Ok(Some(u)) if u.org_id == auth.org_id => {}
        Ok(Some(_)) | Ok(None) => {
            return (
                StatusCode::NOT_FOUND,
                Json(json!({"error": "User not found"})),
            )
                .into_response();
        }
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(json!({"error": format!("Database error: {}", e)})),
            )
                .into_response();
        }
    }

    match db::roles::assign_role(&state.pool, user_id, role_id).await {
        Ok(_) => {
            let _ = db::audit::log_action(
                &state.pool,
                auth.org_id,
                auth.user_id,
                "assign_role",
                Some("role"),
                Some(role_id),
                Some(json!({"user_id": user_id})),
                None,
            )
            .await;

            Json(json!({"status": "ok"})).into_response()
        }
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(json!({"error": format!("Failed to assign role: {}", e)})),
        )
            .into_response(),
    }
}

pub async fn remove_role(
    State(state): State<AppState>,
    auth: AdminAuth,
    Path((role_id, user_id)): Path<(i32, i32)>,
) -> impl IntoResponse {
    match db::roles::remove_role(&state.pool, user_id, role_id).await {
        Ok(_) => {
            let _ = db::audit::log_action(
                &state.pool,
                auth.org_id,
                auth.user_id,
                "remove_role",
                Some("role"),
                Some(role_id),
                Some(json!({"user_id": user_id})),
                None,
            )
            .await;

            Json(json!({"status": "ok"})).into_response()
        }
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(json!({"error": format!("Failed to remove role: {}", e)})),
        )
            .into_response(),
    }
}
