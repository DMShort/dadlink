use serde::Deserialize;

#[derive(Debug, Deserialize, Clone)]
pub struct AppConfig {
    #[serde(default)]
    pub server: ServerConfig,
    #[serde(default)]
    pub database: DatabaseConfig,
    #[serde(default)]
    pub security: SecurityConfig,
    #[serde(default)]
    pub bootstrap: BootstrapConfig,
    #[serde(default)]
    pub audit: AuditConfig,
    #[serde(default)]
    pub voice: VoiceConfig,
    #[serde(default)]
    pub redis: RedisConfig,
    #[serde(default)]
    pub discord: DiscordConfig,
}

/// "Login with Discord". Disabled unless client_id, client_secret and redirect_uri are set.
#[derive(Debug, Deserialize, Clone)]
pub struct DiscordConfig {
    #[serde(default)]
    pub client_id: Option<String>,
    #[serde(default)]
    pub client_secret: Option<String>,
    /// Public URL of this server's callback, registered in the Discord developer portal,
    /// e.g. https://voice.example.org:9000/auth/discord/callback
    #[serde(default)]
    pub redirect_uri: Option<String>,
    /// If set, only members of this Discord server (guild) may log in, and their Discord
    /// roles are synced to DadLink roles that carry a matching discord_role_id.
    #[serde(default)]
    pub guild_id: Option<String>,
    /// Create a DadLink account on first Discord login (otherwise an admin must link one).
    #[serde(default = "default_true")]
    pub auto_create_users: bool,
    /// Overridable for tests.
    #[serde(default = "default_discord_api")]
    pub api_base: String,
    #[serde(default = "default_discord_authorize")]
    pub authorize_url: String,
}

impl Default for DiscordConfig {
    fn default() -> Self {
        Self {
            client_id: None,
            client_secret: None,
            redirect_uri: None,
            guild_id: None,
            auto_create_users: true,
            api_base: default_discord_api(),
            authorize_url: default_discord_authorize(),
        }
    }
}

impl DiscordConfig {
    pub fn enabled(&self) -> bool {
        self.client_id.as_deref().map_or(false, |s| !s.is_empty())
            && self.client_secret.as_deref().map_or(false, |s| !s.is_empty())
            && self.redirect_uri.as_deref().map_or(false, |s| !s.is_empty())
    }
}

fn default_true() -> bool { true }
fn default_discord_api() -> String { "https://discord.com/api/v10".into() }
fn default_discord_authorize() -> String { "https://discord.com/oauth2/authorize".into() }

#[derive(Debug, Deserialize, Clone)]
pub struct ServerConfig {
    #[serde(default = "default_bind_address")]
    pub bind_address: String,
    #[serde(default = "default_control_port")]
    pub control_port: u16,
    #[serde(default = "default_voice_port")]
    pub voice_port: u16,
    #[serde(default)]
    pub tls_cert_path: Option<String>,
    #[serde(default)]
    pub tls_key_path: Option<String>,
    #[serde(default)]
    pub allowed_origins: Vec<String>,
    /// Latest client version — clients are notified if they're outdated
    #[serde(default)]
    pub client_version: Option<String>,
    /// Download URL for client updates
    #[serde(default)]
    pub client_download_url: Option<String>,
}

#[derive(Debug, Deserialize, Clone)]
pub struct DatabaseConfig {
    #[serde(default = "default_database_url")]
    pub url: String,
    #[serde(default = "default_max_connections")]
    pub max_connections: u32,
}

#[derive(Debug, Deserialize, Clone)]
pub struct SecurityConfig {
    #[serde(default = "default_jwt_secret")]
    pub jwt_secret: String,
    #[serde(default = "default_jwt_expiration_secs")]
    pub jwt_expiration_secs: u64,
    #[serde(default = "default_key_rotation_mins")]
    pub key_rotation_mins: u64,
}

#[derive(Debug, Deserialize, Clone, Default)]
pub struct BootstrapConfig {
    pub org_name: Option<String>,
    pub org_tag: Option<String>,
    pub admin_username: Option<String>,
    pub admin_password: Option<String>,
}

#[derive(Debug, Deserialize, Clone)]
pub struct AuditConfig {
    #[serde(default = "default_retention_days")]
    pub retention_days: u32,
}

#[derive(Debug, Deserialize, Clone)]
pub struct VoiceConfig {
    #[serde(default = "default_max_packet_rate")]
    pub max_packet_rate: u32,
    #[serde(default = "default_buffer_pool_size")]
    pub buffer_pool_size: usize,
}

#[derive(Debug, Deserialize, Clone, Default)]
pub struct RedisConfig {
    pub url: Option<String>,
}

// Defaults
fn default_bind_address() -> String {
    "0.0.0.0".to_string()
}
fn default_control_port() -> u16 {
    9000
}
fn default_voice_port() -> u16 {
    9001
}
fn default_database_url() -> String {
    "postgresql://voip:voip@localhost:5432/voip".to_string()
}
fn default_max_connections() -> u32 {
    20
}
fn default_jwt_secret() -> String {
    "CHANGE_ME_IN_PRODUCTION".to_string()
}
fn default_jwt_expiration_secs() -> u64 {
    3600
}
fn default_key_rotation_mins() -> u64 {
    30
}
fn default_retention_days() -> u32 {
    90
}
fn default_max_packet_rate() -> u32 {
    100
}
fn default_buffer_pool_size() -> usize {
    256
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            bind_address: default_bind_address(),
            control_port: default_control_port(),
            voice_port: default_voice_port(),
            tls_cert_path: None,
            tls_key_path: None,
            allowed_origins: Vec::new(),
            client_version: None,
            client_download_url: None,
        }
    }
}

impl Default for DatabaseConfig {
    fn default() -> Self {
        Self {
            url: default_database_url(),
            max_connections: default_max_connections(),
        }
    }
}

impl Default for SecurityConfig {
    fn default() -> Self {
        Self {
            jwt_secret: default_jwt_secret(),
            jwt_expiration_secs: default_jwt_expiration_secs(),
            key_rotation_mins: default_key_rotation_mins(),
        }
    }
}

impl Default for AuditConfig {
    fn default() -> Self {
        Self {
            retention_days: default_retention_days(),
        }
    }
}

impl Default for VoiceConfig {
    fn default() -> Self {
        Self {
            max_packet_rate: default_max_packet_rate(),
            buffer_pool_size: default_buffer_pool_size(),
        }
    }
}

impl AppConfig {
    pub fn load() -> anyhow::Result<Self> {
        let config = config::Config::builder()
            .add_source(config::File::with_name("config/server").required(false))
            .add_source(
                config::Environment::with_prefix("VOIP")
                    .separator("__")
                    .try_parsing(true),
            )
            .build()?;

        let app_config: AppConfig = config.try_deserialize()?;
        Ok(app_config)
    }
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            server: ServerConfig::default(),
            database: DatabaseConfig::default(),
            security: SecurityConfig::default(),
            bootstrap: BootstrapConfig::default(),
            audit: AuditConfig::default(),
            voice: VoiceConfig::default(),
            redis: RedisConfig::default(),
            discord: DiscordConfig::default(),
        }
    }
}
