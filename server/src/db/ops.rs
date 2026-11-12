//! Op templates (reusable comms layouts) and live ops (temporary channels).

use std::collections::HashMap;

use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use sqlx::{FromRow, PgPool};

use super::models::Channel;

/// Access a role gets on a net. `Talk` implies `Listen`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, PartialOrd, Ord)]
#[serde(rename_all = "lowercase")]
pub enum NetAccess {
    Listen,
    Talk,
}

impl NetAccess {
    pub fn as_str(self) -> &'static str {
        match self { NetAccess::Listen => "listen", NetAccess::Talk => "talk" }
    }
    pub fn parse(s: &str) -> Option<Self> {
        match s { "listen" => Some(NetAccess::Listen), "talk" => Some(NetAccess::Talk), _ => None }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RoleAccess {
    pub role_id: i32,
    pub access: NetAccess,
}

/// A net as defined in a template (input and output shape).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NetSpec {
    #[serde(default, skip_deserializing)]
    pub id: i32,
    pub name: String,
    #[serde(default)]
    pub description: Option<String>,
    #[serde(default)]
    pub position: i32,
    #[serde(default = "default_priority")]
    pub priority: i32,
    #[serde(default)]
    pub pan: f32,
    #[serde(default)]
    pub radio: bool,
    #[serde(default)]
    pub roles: Vec<RoleAccess>,
}

fn default_priority() -> i32 { 5 }

#[derive(Debug, Clone, Serialize)]
pub struct Template {
    pub id: i32,
    pub org_id: i32,
    pub name: String,
    pub description: Option<String>,
    pub created_by: Option<i32>,
    pub updated_at: Option<DateTime<Utc>>,
    pub nets: Vec<NetSpec>,
}

#[derive(Debug, Clone, FromRow)]
struct TemplateRow {
    id: i32,
    org_id: i32,
    name: String,
    description: Option<String>,
    created_by: Option<i32>,
    updated_at: Option<DateTime<Utc>>,
}

#[derive(Debug, Clone, FromRow)]
struct NetRow {
    id: i32,
    template_id: i32,
    name: String,
    description: Option<String>,
    position: i32,
    priority: i32,
    pan: f32,
    radio: bool,
}

/// A live op's net (a real channel).
#[derive(Debug, Clone, Serialize)]
pub struct OpNet {
    pub channel_id: i32,
    pub name: String,
    pub position: i32,
    pub priority: i32,
    pub pan: f32,
    pub radio: bool,
    pub roles: Vec<RoleAccess>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Op {
    pub id: i32,
    pub org_id: i32,
    pub template_id: Option<i32>,
    pub name: String,
    pub parent_channel_id: Option<i32>,
    pub started_by: Option<i32>,
    pub started_at: Option<DateTime<Utc>>,
    pub nets: Vec<OpNet>,
}

#[derive(Debug, Clone, FromRow)]
struct OpRow {
    id: i32,
    org_id: i32,
    template_id: Option<i32>,
    name: String,
    parent_channel_id: Option<i32>,
    started_by: Option<i32>,
    started_at: Option<DateTime<Utc>>,
}

// ─── Templates ───

async fn load_template_nets(pool: &PgPool, template_ids: &[i32]) -> anyhow::Result<HashMap<i32, Vec<NetSpec>>> {
    let nets = sqlx::query_as::<_, NetRow>(
        "SELECT id, template_id, name, description, position, priority, pan, radio
         FROM op_template_nets WHERE template_id = ANY($1) ORDER BY position, id",
    )
    .bind(template_ids)
    .fetch_all(pool)
    .await?;

    let net_ids: Vec<i32> = nets.iter().map(|n| n.id).collect();
    let roles: Vec<(i32, i32, String)> = sqlx::query_as(
        "SELECT net_id, role_id, access FROM op_template_net_roles WHERE net_id = ANY($1) ORDER BY role_id",
    )
    .bind(&net_ids)
    .fetch_all(pool)
    .await?;

    let mut roles_by_net: HashMap<i32, Vec<RoleAccess>> = HashMap::new();
    for (net_id, role_id, access) in roles {
        if let Some(access) = NetAccess::parse(&access) {
            roles_by_net.entry(net_id).or_default().push(RoleAccess { role_id, access });
        }
    }

    let mut out: HashMap<i32, Vec<NetSpec>> = HashMap::new();
    for n in nets {
        out.entry(n.template_id).or_default().push(NetSpec {
            id: n.id,
            roles: roles_by_net.remove(&n.id).unwrap_or_default(),
            name: n.name,
            description: n.description,
            position: n.position,
            priority: n.priority,
            pan: n.pan,
            radio: n.radio,
        });
    }
    Ok(out)
}

pub async fn list_templates(pool: &PgPool, org_id: i32) -> anyhow::Result<Vec<Template>> {
    let rows = sqlx::query_as::<_, TemplateRow>(
        "SELECT id, org_id, name, description, created_by, updated_at
         FROM op_templates WHERE org_id = $1 ORDER BY name",
    )
    .bind(org_id)
    .fetch_all(pool)
    .await?;
    let ids: Vec<i32> = rows.iter().map(|r| r.id).collect();
    let mut nets = load_template_nets(pool, &ids).await?;
    Ok(rows
        .into_iter()
        .map(|r| Template {
            nets: nets.remove(&r.id).unwrap_or_default(),
            id: r.id,
            org_id: r.org_id,
            name: r.name,
            description: r.description,
            created_by: r.created_by,
            updated_at: r.updated_at,
        })
        .collect())
}

pub async fn get_template(pool: &PgPool, org_id: i32, template_id: i32) -> anyhow::Result<Option<Template>> {
    Ok(list_templates(pool, org_id).await?.into_iter().find(|t| t.id == template_id))
}

async fn insert_nets(
    tx: &mut sqlx::Transaction<'_, sqlx::Postgres>,
    template_id: i32,
    nets: &[NetSpec],
) -> anyhow::Result<()> {
    for (i, net) in nets.iter().enumerate() {
        let (net_id,): (i32,) = sqlx::query_as(
            "INSERT INTO op_template_nets (template_id, name, description, position, priority, pan, radio)
             VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING id",
        )
        .bind(template_id)
        .bind(&net.name)
        .bind(&net.description)
        .bind(if net.position != 0 { net.position } else { i as i32 })
        .bind(net.priority)
        .bind(net.pan)
        .bind(net.radio)
        .fetch_one(&mut **tx)
        .await?;
        for ra in &net.roles {
            sqlx::query("INSERT INTO op_template_net_roles (net_id, role_id, access) VALUES ($1, $2, $3)")
                .bind(net_id)
                .bind(ra.role_id)
                .bind(ra.access.as_str())
                .execute(&mut **tx)
                .await?;
        }
    }
    Ok(())
}

pub async fn create_template(
    pool: &PgPool,
    org_id: i32,
    created_by: i32,
    name: &str,
    description: Option<&str>,
    nets: &[NetSpec],
) -> anyhow::Result<i32> {
    let mut tx = pool.begin().await?;
    let (id,): (i32,) = sqlx::query_as(
        "INSERT INTO op_templates (org_id, name, description, created_by) VALUES ($1, $2, $3, $4) RETURNING id",
    )
    .bind(org_id)
    .bind(name)
    .bind(description)
    .bind(created_by)
    .fetch_one(&mut *tx)
    .await?;
    insert_nets(&mut tx, id, nets).await?;
    tx.commit().await?;
    Ok(id)
}

/// Replace a template's name, description and full net list.
pub async fn update_template(
    pool: &PgPool,
    template_id: i32,
    name: &str,
    description: Option<&str>,
    nets: &[NetSpec],
) -> anyhow::Result<()> {
    let mut tx = pool.begin().await?;
    sqlx::query("UPDATE op_templates SET name = $2, description = $3, updated_at = NOW() WHERE id = $1")
        .bind(template_id)
        .bind(name)
        .bind(description)
        .execute(&mut *tx)
        .await?;
    sqlx::query("DELETE FROM op_template_nets WHERE template_id = $1")
        .bind(template_id)
        .execute(&mut *tx)
        .await?;
    insert_nets(&mut tx, template_id, nets).await?;
    tx.commit().await?;
    Ok(())
}

pub async fn delete_template(pool: &PgPool, template_id: i32) -> anyhow::Result<()> {
    sqlx::query("DELETE FROM op_templates WHERE id = $1")
        .bind(template_id)
        .execute(pool)
        .await?;
    Ok(())
}

/// How many of these role ids belong to the org (for validation).
pub async fn count_org_roles(pool: &PgPool, org_id: i32, role_ids: &[i32]) -> anyhow::Result<i64> {
    let (n,): (i64,) = sqlx::query_as("SELECT COUNT(*) FROM roles WHERE org_id = $1 AND id = ANY($2)")
        .bind(org_id)
        .bind(role_ids)
        .fetch_one(pool)
        .await?;
    Ok(n)
}

// ─── Live ops ───

/// Result of starting an op: the op plus every channel it created (parent first).
pub struct StartedOp {
    pub op_id: i32,
    pub channels: Vec<Channel>,
}

/// Create the op's channels (parent "lobby" + one child per net), their ACLs and the op rows,
/// all in one transaction.
pub async fn start_op(
    pool: &PgPool,
    org_id: i32,
    started_by: i32,
    name: &str,
    template: &Template,
) -> anyhow::Result<StartedOp> {
    let mut tx = pool.begin().await?;

    // Lobby/briefing channel: no ACL, so anyone in the org can drop in.
    let parent = sqlx::query_as::<_, Channel>(
        "INSERT INTO channels (org_id, parent_id, name, description, position, max_users)
         VALUES ($1, NULL, $2, $3, 1000, 0) RETURNING *",
    )
    .bind(org_id)
    .bind(name)
    .bind(format!("Op from template '{}'", template.name))
    .fetch_one(&mut *tx)
    .await?;

    let (op_id,): (i32,) = sqlx::query_as(
        "INSERT INTO ops (org_id, template_id, name, parent_channel_id, started_by)
         VALUES ($1, $2, $3, $4, $5) RETURNING id",
    )
    .bind(org_id)
    .bind(template.id)
    .bind(name)
    .bind(parent.id)
    .bind(started_by)
    .fetch_one(&mut *tx)
    .await?;

    let mut channels = vec![parent.clone()];
    for net in &template.nets {
        let ch = sqlx::query_as::<_, Channel>(
            "INSERT INTO channels (org_id, parent_id, name, description, position, max_users)
             VALUES ($1, $2, $3, $4, $5, 0) RETURNING *",
        )
        .bind(org_id)
        .bind(parent.id)
        .bind(&net.name)
        .bind(&net.description)
        .bind(net.position)
        .fetch_one(&mut *tx)
        .await?;

        sqlx::query("INSERT INTO op_nets (channel_id, op_id, priority, pan, radio) VALUES ($1, $2, $3, $4, $5)")
            .bind(ch.id)
            .bind(op_id)
            .bind(net.priority)
            .bind(net.pan)
            .bind(net.radio)
            .execute(&mut *tx)
            .await?;

        // A net with no roles assigned stays open to everyone (no ACL rows).
        let acl: Vec<(i32, bool, bool)> = net
            .roles
            .iter()
            .map(|r| (r.role_id, true, r.access == NetAccess::Talk))
            .collect();
        if !acl.is_empty() {
            super::acl::set_channel_acl(&mut tx, ch.id, &acl).await?;
        }
        channels.push(ch);
    }

    tx.commit().await?;
    Ok(StartedOp { op_id, channels })
}

pub async fn list_active_ops(pool: &PgPool, org_id: i32) -> anyhow::Result<Vec<Op>> {
    let rows = sqlx::query_as::<_, OpRow>(
        "SELECT id, org_id, template_id, name, parent_channel_id, started_by, started_at
         FROM ops WHERE org_id = $1 AND ended_at IS NULL ORDER BY started_at",
    )
    .bind(org_id)
    .fetch_all(pool)
    .await?;
    let ids: Vec<i32> = rows.iter().map(|r| r.id).collect();

    let nets: Vec<(i32, i32, String, i32, i32, f32, bool)> = sqlx::query_as(
        "SELECT n.op_id, c.id, c.name, COALESCE(c.position, 0), n.priority, n.pan, n.radio
         FROM op_nets n JOIN channels c ON c.id = n.channel_id
         WHERE n.op_id = ANY($1) ORDER BY c.position, c.id",
    )
    .bind(&ids)
    .fetch_all(pool)
    .await?;

    let channel_ids: Vec<i32> = nets.iter().map(|n| n.1).collect();
    let acl: Vec<(i32, i32, bool)> = sqlx::query_as(
        "SELECT channel_id, role_id, allow_speak FROM channel_acl WHERE channel_id = ANY($1) ORDER BY role_id",
    )
    .bind(&channel_ids)
    .fetch_all(pool)
    .await?;
    let mut roles_by_channel: HashMap<i32, Vec<RoleAccess>> = HashMap::new();
    for (ch, role_id, speak) in acl {
        roles_by_channel.entry(ch).or_default().push(RoleAccess {
            role_id,
            access: if speak { NetAccess::Talk } else { NetAccess::Listen },
        });
    }

    let mut nets_by_op: HashMap<i32, Vec<OpNet>> = HashMap::new();
    for (op_id, channel_id, name, position, priority, pan, radio) in nets {
        nets_by_op.entry(op_id).or_default().push(OpNet {
            roles: roles_by_channel.remove(&channel_id).unwrap_or_default(),
            channel_id, name, position, priority, pan, radio,
        });
    }

    Ok(rows
        .into_iter()
        .map(|r| Op {
            nets: nets_by_op.remove(&r.id).unwrap_or_default(),
            id: r.id,
            org_id: r.org_id,
            template_id: r.template_id,
            name: r.name,
            parent_channel_id: r.parent_channel_id,
            started_by: r.started_by,
            started_at: r.started_at,
        })
        .collect())
}

/// Mark the op ended and delete its channels (children first, then the lobby).
/// Returns the deleted channel ids.
pub async fn end_op(pool: &PgPool, op: &Op) -> anyhow::Result<Vec<i32>> {
    let mut tx = pool.begin().await?;
    sqlx::query("UPDATE ops SET ended_at = NOW() WHERE id = $1")
        .bind(op.id)
        .execute(&mut *tx)
        .await?;

    let mut deleted: Vec<i32> = Vec::new();
    if let Some(parent) = op.parent_channel_id {
        // Every child of the lobby, including any channel an admin added under it by hand
        let children: Vec<(i32,)> = sqlx::query_as("SELECT id FROM channels WHERE parent_id = $1")
            .bind(parent)
            .fetch_all(&mut *tx)
            .await?;
        for (id,) in &children {
            deleted.push(*id);
        }
        sqlx::query("DELETE FROM channels WHERE parent_id = $1")
            .bind(parent)
            .execute(&mut *tx)
            .await?;
        sqlx::query("DELETE FROM channels WHERE id = $1")
            .bind(parent)
            .execute(&mut *tx)
            .await?;
        deleted.push(parent);
    }
    tx.commit().await?;
    Ok(deleted)
}

/// Role ids held by a user.
pub async fn user_role_ids(pool: &PgPool, user_id: i32) -> anyhow::Result<Vec<i32>> {
    let rows: Vec<(i32,)> = sqlx::query_as("SELECT role_id FROM user_roles WHERE user_id = $1")
        .bind(user_id)
        .fetch_all(pool)
        .await?;
    Ok(rows.into_iter().map(|(r,)| r).collect())
}

/// Is the user an admin (flag or a role carrying the Admin bit)?
pub async fn user_is_admin(pool: &PgPool, user_id: i32) -> anyhow::Result<bool> {
    let (admin,): (bool,) = sqlx::query_as(
        "SELECT COALESCE((SELECT is_admin FROM users WHERE id = $1), FALSE)
           OR EXISTS (SELECT 1 FROM roles r JOIN user_roles ur ON ur.role_id = r.id
                      WHERE ur.user_id = $1 AND (r.permissions & 128) <> 0)",
    )
    .bind(user_id)
    .fetch_one(pool)
    .await?;
    Ok(admin)
}
