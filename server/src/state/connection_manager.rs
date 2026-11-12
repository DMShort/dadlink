use std::collections::HashSet;
use std::net::SocketAddr;

use dashmap::DashMap;
use tokio::sync::mpsc;
use tracing::debug;

use crate::server::messages::Envelope;

/// Handle to send messages to a connected user's WebSocket.
#[derive(Debug, Clone)]
pub struct ConnectionHandle {
    pub user_id: i32,
    pub org_id: i32,
    pub username: String,
    pub display_name: Option<String>,
    pub channels: HashSet<i32>,
    pub tx: mpsc::UnboundedSender<String>,
    pub udp_addr: Option<SocketAddr>,
}

/// Manages all active WebSocket connections.
pub struct ConnectionManager {
    connections: DashMap<i32, ConnectionHandle>,
}

impl ConnectionManager {
    pub fn new() -> Self {
        Self {
            connections: DashMap::new(),
        }
    }

    pub fn add(&self, handle: ConnectionHandle) {
        debug!("Connection added: user_id={}", handle.user_id);
        self.connections.insert(handle.user_id, handle);
    }

    pub fn remove(&self, user_id: i32) -> Option<ConnectionHandle> {
        debug!("Connection removed: user_id={}", user_id);
        self.connections.remove(&user_id).map(|(_, h)| h)
    }

    pub fn get(&self, user_id: i32) -> Option<ConnectionHandle> {
        self.connections.get(&user_id).map(|h| h.clone())
    }

    pub fn add_channel(&self, user_id: i32, channel_id: i32) {
        if let Some(mut handle) = self.connections.get_mut(&user_id) {
            handle.channels.insert(channel_id);
        }
    }

    pub fn remove_channel(&self, user_id: i32, channel_id: i32) {
        if let Some(mut handle) = self.connections.get_mut(&user_id) {
            handle.channels.remove(&channel_id);
        }
    }

    pub fn set_udp_addr(&self, user_id: i32, addr: SocketAddr) {
        if let Some(mut handle) = self.connections.get_mut(&user_id) {
            handle.udp_addr = Some(addr);
        }
    }

    /// Send a message to a specific user.
    pub fn send_to(&self, user_id: i32, msg: &str) {
        if let Some(handle) = self.connections.get(&user_id) {
            let _ = handle.tx.send(msg.to_string());
        }
    }

    /// Broadcast a message to all users in a channel (optionally excluding one).
    pub fn broadcast_to_channel(&self, channel_id: i32, msg: &str, exclude: Option<i32>) {
        for entry in self.connections.iter() {
            let handle = entry.value();
            if handle.channels.contains(&channel_id) {
                if exclude.map_or(true, |ex| ex != handle.user_id) {
                    let _ = handle.tx.send(msg.to_string());
                }
            }
        }
    }

    /// Broadcast to all users in an organization.
    pub fn broadcast_to_org(&self, org_id: i32, msg: &str) {
        for entry in self.connections.iter() {
            let handle = entry.value();
            if handle.org_id == org_id {
                let _ = handle.tx.send(msg.to_string());
            }
        }
    }

    /// All connected user IDs in an org.
    pub fn users_in_org(&self, org_id: i32) -> Vec<i32> {
        self.connections
            .iter()
            .filter(|e| e.value().org_id == org_id)
            .map(|e| *e.key())
            .collect()
    }

    /// Get all user IDs in a channel.
    pub fn users_in_channel(&self, channel_id: i32) -> Vec<i32> {
        self.connections
            .iter()
            .filter(|e| e.value().channels.contains(&channel_id))
            .map(|e| *e.key())
            .collect()
    }

    /// Send a typed message to a user.
    pub fn send_envelope(&self, user_id: i32, envelope: &Envelope) {
        self.send_to(user_id, &envelope.to_json());
    }

    pub fn active_count(&self) -> usize {
        self.connections.len()
    }

    /// Get all connected user IDs.
    pub fn all_user_ids(&self) -> Vec<i32> {
        self.connections.iter().map(|e| *e.key()).collect()
    }
}
