use serde::{Deserialize, Serialize};

/// Permission bitflags for role-based access control.
/// Each permission is a single bit in a 32-bit integer.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[repr(i32)]
pub enum Permission {
    CanJoin = 1,
    CanSpeak = 2,
    CanMoveUsers = 4,
    CanManageChannels = 8,
    CanManageUsers = 16,
    CanManageRoles = 32,
    CanViewAuditLog = 64,
    Admin = 128,
    /// Create/edit op templates and start/end ops (without full admin rights).
    OpLeader = 256,
}

impl Permission {
    pub const ALL: &'static [Permission] = &[
        Permission::CanJoin,
        Permission::CanSpeak,
        Permission::CanMoveUsers,
        Permission::CanManageChannels,
        Permission::CanManageUsers,
        Permission::CanManageRoles,
        Permission::CanViewAuditLog,
        Permission::Admin,
        Permission::OpLeader,
    ];
}

/// Check if a permission bitmask contains a specific permission.
pub fn has_permission(bitmask: i32, perm: Permission) -> bool {
    // Admin implies all permissions
    if bitmask & Permission::Admin as i32 != 0 {
        return true;
    }
    bitmask & perm as i32 != 0
}

/// Compute the effective permission bitmask from a list of role permission values.
pub fn compute_permissions(role_permissions: &[i32]) -> i32 {
    role_permissions.iter().fold(0, |acc, &p| acc | p)
}

/// Check if a user (identified by is_admin flag and role permissions) has a given permission.
/// The is_admin flag on the user record grants full access regardless of roles.
pub fn user_has_permission(is_admin: bool, role_permissions: &[i32], perm: Permission) -> bool {
    if is_admin {
        return true;
    }
    let effective = compute_permissions(role_permissions);
    has_permission(effective, perm)
}
