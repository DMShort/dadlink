use axum::{
    async_trait,
    extract::FromRequestParts,
    http::{request::Parts, StatusCode},
    response::{IntoResponse, Json, Response},
};
use serde_json::json;

use crate::auth::jwt;
use crate::auth::permissions::{Permission, has_permission, compute_permissions};
use crate::server::AppState;

/// Authenticated admin context extracted from the Authorization header.
/// Include as a handler parameter to require admin access.
#[derive(Debug, Clone)]
pub struct AdminAuth {
    pub user_id: i32,
    pub org_id: i32,
    pub is_admin: bool,
    pub permissions: i32,
}

impl AdminAuth {
    pub fn has_permission(&self, perm: Permission) -> bool {
        self.is_admin || has_permission(self.permissions, perm)
    }
}

#[async_trait]
impl FromRequestParts<AppState> for AdminAuth {
    type Rejection = Response;

    async fn from_request_parts(parts: &mut Parts, state: &AppState) -> Result<Self, Self::Rejection> {
        authenticate(parts, state, Permission::Admin, "Admin access required").await
    }
}

/// Extractor for the ops API: allows admins and anyone with the OpLeader permission.
#[derive(Debug, Clone)]
pub struct OpLeaderAuth(pub AdminAuth);

#[async_trait]
impl FromRequestParts<AppState> for OpLeaderAuth {
    type Rejection = Response;

    async fn from_request_parts(parts: &mut Parts, state: &AppState) -> Result<Self, Self::Rejection> {
        authenticate(parts, state, Permission::OpLeader, "Op Leader permission required")
            .await
            .map(OpLeaderAuth)
    }
}

/// Validate the bearer token and session, then require `required` (Admin implies everything).
async fn authenticate(
    parts: &mut Parts,
    state: &AppState,
    required: Permission,
    denied_message: &str,
) -> Result<AdminAuth, Response> {
    {
        // Get Authorization header
        let auth_header = parts
            .headers
            .get("authorization")
            .and_then(|v| v.to_str().ok())
            .ok_or_else(|| {
                (
                    StatusCode::UNAUTHORIZED,
                    Json(json!({"error": "Missing Authorization header"})),
                )
                    .into_response()
            })?;

        // Extract Bearer token
        let token = auth_header
            .strip_prefix("Bearer ")
            .ok_or_else(|| {
                (
                    StatusCode::UNAUTHORIZED,
                    Json(json!({"error": "Invalid Authorization format, expected: Bearer <token>"})),
                )
                    .into_response()
            })?;

        // Validate JWT
        let claims = jwt::validate_token(token, &state.config.security.jwt_secret).map_err(|_| {
            (
                StatusCode::UNAUTHORIZED,
                Json(json!({"error": "Invalid or expired token"})),
            )
                .into_response()
        })?;

        // Check session is not revoked
        let jti: uuid::Uuid = claims.jti.parse().map_err(|_| {
            (
                StatusCode::UNAUTHORIZED,
                Json(json!({"error": "Invalid token"})),
            )
                .into_response()
        })?;

        let valid = state
            .session_cache
            .is_session_valid(&state.pool, jti)
            .await;

        if !valid {
            return Err((
                StatusCode::UNAUTHORIZED,
                Json(json!({"error": "Session has been revoked"})),
            )
                .into_response());
        }

        // Look up user to get is_admin flag
        let user = crate::db::users::find_by_id(&state.pool, claims.sub)
            .await
            .map_err(|_| {
                (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    Json(json!({"error": "Database error"})),
                )
                    .into_response()
            })?
            .ok_or_else(|| {
                (
                    StatusCode::UNAUTHORIZED,
                    Json(json!({"error": "User not found"})),
                )
                    .into_response()
            })?;

        let is_admin = user.is_admin.unwrap_or(false);

        // Get role permissions
        let role_perms = crate::db::roles::get_user_permissions(&state.pool, claims.sub)
            .await
            .unwrap_or_default();

        let effective_perms = compute_permissions(&role_perms);

        // Must have the required permission (Admin implies all) or the is_admin flag
        if !is_admin && !has_permission(effective_perms, required) {
            return Err((
                StatusCode::FORBIDDEN,
                Json(json!({"error": denied_message})),
            )
                .into_response());
        }

        // Admin API rate limiting: 60 req/min per user
        if !crate::server::rate_limit::admin_limiter().check_and_consume(claims.sub) {
            crate::server::metrics::metrics().admin_requests_rate_limited.inc();
            return Err((
                StatusCode::TOO_MANY_REQUESTS,
                Json(json!({"error": "Rate limit exceeded. Try again later."})),
            )
                .into_response());
        }

        Ok(AdminAuth {
            user_id: claims.sub,
            org_id: claims.org,
            is_admin,
            permissions: if is_admin { 0x1FF } else { effective_perms },
        })
    }
}
