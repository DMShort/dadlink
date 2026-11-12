use std::sync::Arc;
use std::time::{Duration, Instant};

use dashmap::DashMap;
use tracing::debug;

use super::srtp_session::SrtpSession;

/// Entry in the session manager — may hold both current and previous keys during rotation.
struct SessionEntry {
    current: Arc<SrtpSession>,
    previous: Option<(Arc<SrtpSession>, Instant)>, // (old session, rotation time)
}

/// Manages per-user SRTP sessions with support for key rotation dual-key window.
pub struct SrtpSessionManager {
    sessions: DashMap<i32, SessionEntry>,
    dual_key_window: Duration,
}

impl SrtpSessionManager {
    pub fn new() -> Self {
        Self {
            sessions: DashMap::new(),
            dual_key_window: Duration::from_secs(2),
        }
    }

    /// Create or replace the SRTP session for a user.
    pub fn set_session(&self, user_id: i32, session: SrtpSession) {
        let new_session = Arc::new(session);

        if let Some(mut entry) = self.sessions.get_mut(&user_id) {
            // Key rotation: move current to previous
            let old = entry.current.clone();
            entry.current = new_session;
            entry.previous = Some((old, Instant::now()));
            debug!("SRTP key rotated for user {}", user_id);
        } else {
            self.sessions.insert(user_id, SessionEntry {
                current: new_session,
                previous: None,
            });
            debug!("SRTP session created for user {}", user_id);
        }
    }

    /// Get the current SRTP session for encryption (always use the latest key).
    pub fn get_encrypt_session(&self, user_id: i32) -> Option<Arc<SrtpSession>> {
        self.sessions.get(&user_id).map(|e| e.current.clone())
    }

    /// Try to decrypt with the current key first, then fall back to the previous key
    /// if within the dual-key window.
    pub fn decrypt(
        &self,
        user_id: i32,
        ciphertext: &[u8],
        sequence: u64,
    ) -> Result<Vec<u8>, super::srtp_session::SrtpError> {
        let entry = self
            .sessions
            .get(&user_id)
            .ok_or(super::srtp_session::SrtpError::DecryptionFailed)?;

        // Try current key first
        if let Ok(plaintext) = entry.current.decrypt(ciphertext, sequence, user_id as u32) {
            return Ok(plaintext);
        }

        // Try previous key if within dual-key window
        if let Some((ref prev, rotation_time)) = entry.previous {
            if rotation_time.elapsed() < self.dual_key_window {
                return prev.decrypt(ciphertext, sequence, user_id as u32);
            }
        }

        Err(super::srtp_session::SrtpError::DecryptionFailed)
    }

    /// Remove a user's SRTP session.
    pub fn remove(&self, user_id: i32) {
        self.sessions.remove(&user_id);
        debug!("SRTP session removed for user {}", user_id);
    }

    /// Check if a user has an active SRTP session.
    pub fn has_session(&self, user_id: i32) -> bool {
        self.sessions.contains_key(&user_id)
    }
}
