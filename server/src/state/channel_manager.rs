use std::collections::HashSet;

use dashmap::DashMap;
use tracing::debug;

/// Tracks which users are in which channels (in-memory, voice routing state).
pub struct ChannelManager {
    /// channel_id -> set of user_ids currently in the channel
    channels: DashMap<i32, HashSet<i32>>,
    /// channel_id -> max_users limit
    limits: DashMap<i32, i32>,
    /// channel_id -> members allowed to transmit (subset of `channels`)
    speakers: DashMap<i32, HashSet<i32>>,
}

impl ChannelManager {
    pub fn new() -> Self {
        Self {
            channels: DashMap::new(),
            limits: DashMap::new(),
            speakers: DashMap::new(),
        }
    }

    /// Register a channel's max_users limit.
    pub fn set_limit(&self, channel_id: i32, max_users: i32) {
        self.limits.insert(channel_id, max_users);
    }

    /// Add a user to a channel. Returns Err if the channel is full.
    pub fn join(&self, channel_id: i32, user_id: i32) -> Result<(), ChannelError> {
        let mut members = self.channels.entry(channel_id).or_default();

        // Check max_users
        if let Some(limit) = self.limits.get(&channel_id) {
            let max = *limit;
            if max > 0 && members.len() >= max as usize {
                return Err(ChannelError::Full);
            }
        }

        members.insert(user_id);
        debug!(
            "User {} joined channel {} ({} members)",
            user_id,
            channel_id,
            members.len()
        );
        Ok(())
    }

    /// Record whether a joined member may transmit in this channel.
    pub fn set_can_speak(&self, channel_id: i32, user_id: i32, can_speak: bool) {
        let mut speakers = self.speakers.entry(channel_id).or_default();
        if can_speak { speakers.insert(user_id); } else { speakers.remove(&user_id); }
    }

    /// True if the user is in the channel and allowed to transmit there.
    pub fn can_speak(&self, channel_id: i32, user_id: i32) -> bool {
        self.speakers
            .get(&channel_id)
            .map(|s| s.contains(&user_id))
            .unwrap_or(false)
    }

    /// Remove a user from a channel.
    pub fn leave(&self, channel_id: i32, user_id: i32) {
        if let Some(mut speakers) = self.speakers.get_mut(&channel_id) {
            speakers.remove(&user_id);
        }
        if let Some(mut members) = self.channels.get_mut(&channel_id) {
            members.remove(&user_id);
            debug!(
                "User {} left channel {} ({} members)",
                user_id,
                channel_id,
                members.len()
            );
        }
    }

    /// Remove a user from all channels. Returns list of channel IDs they were in.
    pub fn leave_all(&self, user_id: i32) -> Vec<i32> {
        let mut left_channels = Vec::new();
        for mut entry in self.channels.iter_mut() {
            if entry.value_mut().remove(&user_id) {
                left_channels.push(*entry.key());
            }
        }
        for mut entry in self.speakers.iter_mut() {
            entry.value_mut().remove(&user_id);
        }
        left_channels
    }

    /// Get all user IDs in a channel.
    pub fn members(&self, channel_id: i32) -> Vec<i32> {
        self.channels
            .get(&channel_id)
            .map(|s| s.iter().copied().collect())
            .unwrap_or_default()
    }

    /// Get member count for a channel.
    pub fn member_count(&self, channel_id: i32) -> usize {
        self.channels
            .get(&channel_id)
            .map(|s| s.len())
            .unwrap_or(0)
    }

    /// Check if a user is in a channel.
    pub fn is_member(&self, channel_id: i32, user_id: i32) -> bool {
        self.channels
            .get(&channel_id)
            .map(|s| s.contains(&user_id))
            .unwrap_or(false)
    }

    /// Remove a channel entirely (e.g., on deletion).
    pub fn remove_channel(&self, channel_id: i32) -> Vec<i32> {
        self.speakers.remove(&channel_id);
        self.channels
            .remove(&channel_id)
            .map(|(_, members)| members.into_iter().collect())
            .unwrap_or_default()
    }
}

#[derive(Debug, thiserror::Error)]
pub enum ChannelError {
    #[error("Channel is full")]
    Full,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn speak_rights_follow_membership() {
        let cm = ChannelManager::new();
        cm.join(1, 10).unwrap();
        cm.set_can_speak(1, 10, false);
        assert!(cm.is_member(1, 10));
        assert!(!cm.can_speak(1, 10), "listen-only member must not transmit");

        cm.set_can_speak(1, 10, true);
        assert!(cm.can_speak(1, 10));

        cm.leave(1, 10);
        assert!(!cm.can_speak(1, 10), "leaving clears speak rights");

        cm.join(2, 10).unwrap();
        cm.set_can_speak(2, 10, true);
        cm.leave_all(10);
        assert!(!cm.can_speak(2, 10), "disconnect clears speak rights");

        cm.join(3, 11).unwrap();
        cm.set_can_speak(3, 11, true);
        cm.remove_channel(3);
        assert!(!cm.can_speak(3, 11), "deleted channel clears speak rights");
        assert!(!cm.can_speak(99, 11), "unknown channel");
    }
}
