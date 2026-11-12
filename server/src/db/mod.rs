pub mod acl;
pub mod audit;
pub mod channels;
pub mod invites;
pub mod models;
pub mod ops;
pub mod organizations;
pub mod roles;
pub mod sessions;
pub mod users;

use sqlx::postgres::PgPoolOptions;
use sqlx::PgPool;
use tracing::info;

pub async fn init_pool(database_url: &str, max_connections: u32) -> anyhow::Result<PgPool> {
    let pool = PgPoolOptions::new()
        .max_connections(max_connections)
        .connect(database_url)
        .await?;

    info!("Database connection pool established");
    Ok(pool)
}

pub async fn run_migrations(pool: &PgPool) -> anyhow::Result<()> {
    info!("Running database migrations...");
    sqlx::migrate!("./migrations").run(pool).await?;
    info!("Database migrations complete");
    Ok(())
}
