use sqlx::PgPool;

/// What a user may do in a channel, from `channel_acl` + their roles.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ChannelAccess {
    pub can_join: bool,
    pub can_speak: bool,
}

/// Resolve channel access for a user.
///
/// - Channels with no ACL rows are open: everyone may join and speak (pre-existing behaviour).
/// - Channels with ACL rows: a user needs a role with `allow_join` to join and
///   `allow_speak` to transmit. Admins (is_admin flag or a role with the Admin bit) bypass ACLs.
pub async fn channel_access(pool: &PgPool, channel_id: i32, user_id: i32) -> anyhow::Result<ChannelAccess> {
    let (is_admin, has_acl, can_join, can_speak): (bool, bool, bool, bool) = sqlx::query_as(
        r#"
        SELECT
            COALESCE((SELECT is_admin FROM users WHERE id = $2), FALSE)
              OR EXISTS (SELECT 1 FROM roles r JOIN user_roles ur ON ur.role_id = r.id
                         WHERE ur.user_id = $2 AND (r.permissions & 128) <> 0),
            EXISTS (SELECT 1 FROM channel_acl WHERE channel_id = $1),
            COALESCE((SELECT bool_or(a.allow_join) FROM channel_acl a
                      JOIN user_roles ur ON ur.role_id = a.role_id
                      WHERE a.channel_id = $1 AND ur.user_id = $2), FALSE),
            COALESCE((SELECT bool_or(a.allow_speak) FROM channel_acl a
                      JOIN user_roles ur ON ur.role_id = a.role_id
                      WHERE a.channel_id = $1 AND ur.user_id = $2), FALSE)
        "#,
    )
    .bind(channel_id)
    .bind(user_id)
    .fetch_one(pool)
    .await?;

    if is_admin || !has_acl {
        return Ok(ChannelAccess { can_join: true, can_speak: true });
    }
    // Speaking implies being allowed in.
    Ok(ChannelAccess { can_join: can_join || can_speak, can_speak })
}

/// Replace all ACL rows for a channel.
pub async fn set_channel_acl(
    tx: &mut sqlx::Transaction<'_, sqlx::Postgres>,
    channel_id: i32,
    rows: &[(i32, bool, bool)], // (role_id, allow_join, allow_speak)
) -> anyhow::Result<()> {
    sqlx::query("DELETE FROM channel_acl WHERE channel_id = $1")
        .bind(channel_id)
        .execute(&mut **tx)
        .await?;
    for (role_id, join, speak) in rows {
        sqlx::query(
            "INSERT INTO channel_acl (channel_id, role_id, allow_join, allow_speak, allow_manage)
             VALUES ($1, $2, $3, $4, FALSE)",
        )
        .bind(channel_id)
        .bind(role_id)
        .bind(join)
        .bind(speak)
        .execute(&mut **tx)
        .await?;
    }
    Ok(())
}
