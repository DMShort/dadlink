-- Discord login: link users to Discord accounts, and map Discord roles to DadLink roles
ALTER TABLE users ADD COLUMN discord_id VARCHAR(32);
CREATE UNIQUE INDEX idx_users_org_discord ON users(org_id, discord_id) WHERE discord_id IS NOT NULL;

ALTER TABLE roles ADD COLUMN discord_role_id VARCHAR(32);
CREATE INDEX idx_roles_discord ON roles(org_id, discord_role_id) WHERE discord_role_id IS NOT NULL;
