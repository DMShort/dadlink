pub mod channels;
pub mod invites;
pub mod middleware;
pub mod ops;
pub mod roles;
pub mod sessions;
pub mod users;

use axum::{routing::{delete, get, post, put}, Router};

use super::AppState;

pub fn admin_routes() -> Router<AppState> {
    Router::new()
        // Users
        .route("/admin/users", get(users::list_users))
        .route("/admin/users", post(users::create_user))
        .route("/admin/users/bulk", post(users::bulk_create_users))
        .route("/admin/users/:user_id", get(users::get_user))
        .route("/admin/users/:user_id", put(users::update_user))
        .route("/admin/users/:user_id", delete(users::delete_user))
        .route("/admin/users/:user_id/sessions", delete(sessions::revoke_user_sessions))
        // Invites
        .route("/admin/invites", get(invites::list_invites))
        .route("/admin/invites", post(invites::create_invite))
        .route("/admin/invites/:code", delete(invites::revoke_invite))
        // Channels
        .route("/admin/channels", get(channels::list_channels))
        .route("/admin/channels", post(channels::create_channel))
        .route("/admin/channels/:channel_id", put(channels::update_channel))
        .route("/admin/channels/:channel_id", delete(channels::delete_channel))
        // Roles
        .route("/admin/roles", get(roles::list_roles))
        .route("/admin/roles", post(roles::create_role))
        .route("/admin/roles/:role_id", put(roles::update_role))
        .route("/admin/roles/:role_id", delete(roles::delete_role))
        .route("/admin/roles/:role_id/assign/:user_id", post(roles::assign_role))
        .route("/admin/roles/:role_id/assign/:user_id", delete(roles::remove_role))
        // Sessions
        .route("/admin/sessions/:session_id", delete(sessions::revoke_session))
        // Audit
        .route("/admin/audit", get(sessions::get_audit_log))
        // Ops (Admin or OpLeader)
        .route("/ops/roles", get(ops::list_roles))
        .route("/ops/templates", get(ops::list_templates))
        .route("/ops/templates", post(ops::create_template))
        .route("/ops/templates/:template_id", put(ops::update_template))
        .route("/ops/templates/:template_id", delete(ops::delete_template))
        .route("/ops", get(ops::list_ops))
        .route("/ops", post(ops::start_op))
        .route("/ops/:op_id", delete(ops::end_op))
}
