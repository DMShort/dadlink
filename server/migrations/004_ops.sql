-- Op templates: a reusable comms layout (nets + which roles get which nets)
CREATE TABLE op_templates (
    id SERIAL PRIMARY KEY,
    org_id INTEGER NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    name VARCHAR(64) NOT NULL,
    description TEXT,
    created_by INTEGER REFERENCES users(id) ON DELETE SET NULL,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW(),
    UNIQUE(org_id, name)
);

-- Nets within a template, with the audio defaults pushed to members' clients
CREATE TABLE op_template_nets (
    id SERIAL PRIMARY KEY,
    template_id INTEGER NOT NULL REFERENCES op_templates(id) ON DELETE CASCADE,
    name VARCHAR(64) NOT NULL,
    description TEXT,
    position INTEGER NOT NULL DEFAULT 0,
    priority INTEGER NOT NULL DEFAULT 5 CHECK (priority BETWEEN 1 AND 10),
    pan REAL NOT NULL DEFAULT 0 CHECK (pan BETWEEN -1 AND 1),
    radio BOOLEAN NOT NULL DEFAULT FALSE,
    UNIQUE(template_id, name)
);
CREATE INDEX idx_op_template_nets_template ON op_template_nets(template_id);

-- Role access per template net: 'listen' or 'talk' (talk implies listen)
CREATE TABLE op_template_net_roles (
    net_id INTEGER NOT NULL REFERENCES op_template_nets(id) ON DELETE CASCADE,
    role_id INTEGER NOT NULL REFERENCES roles(id) ON DELETE CASCADE,
    access VARCHAR(8) NOT NULL CHECK (access IN ('listen', 'talk')),
    PRIMARY KEY(net_id, role_id)
);

-- Live ops. Each op owns a parent channel (the op "lobby") and one child channel per net.
CREATE TABLE ops (
    id SERIAL PRIMARY KEY,
    org_id INTEGER NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    template_id INTEGER REFERENCES op_templates(id) ON DELETE SET NULL,
    name VARCHAR(64) NOT NULL,
    parent_channel_id INTEGER REFERENCES channels(id) ON DELETE SET NULL,
    started_by INTEGER REFERENCES users(id) ON DELETE SET NULL,
    started_at TIMESTAMPTZ DEFAULT NOW(),
    ended_at TIMESTAMPTZ
);
CREATE INDEX idx_ops_active ON ops(org_id) WHERE ended_at IS NULL;

-- Channels created for an op, with the audio defaults copied from the template
CREATE TABLE op_nets (
    channel_id INTEGER PRIMARY KEY REFERENCES channels(id) ON DELETE CASCADE,
    op_id INTEGER NOT NULL REFERENCES ops(id) ON DELETE CASCADE,
    priority INTEGER NOT NULL DEFAULT 5,
    pan REAL NOT NULL DEFAULT 0,
    radio BOOLEAN NOT NULL DEFAULT FALSE
);
CREATE INDEX idx_op_nets_op ON op_nets(op_id);
