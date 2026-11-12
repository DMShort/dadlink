use std::sync::Arc;

use tokio::net::UdpSocket;
use tracing::{debug, trace};

use crate::crypto::session_manager::SrtpSessionManager;
use crate::state::channel_manager::ChannelManager;
use crate::state::connection_manager::ConnectionManager;

use super::buffer_pool::BufferPool;
use super::packet::VoicePacket;
use super::rate_limiter::VoiceRateLimiter;

/// Start the UDP voice routing server.
pub async fn run_voice_server(
    bind_addr: String,
    srtp_manager: Arc<SrtpSessionManager>,
    channel_manager: Arc<ChannelManager>,
    connection_manager: Arc<ConnectionManager>,
    rate_limiter: Arc<VoiceRateLimiter>,
    buffer_pool: Arc<BufferPool>,
) -> anyhow::Result<()> {
    let socket = Arc::new(UdpSocket::bind(&bind_addr).await?);
    tracing::info!("UDP voice server listening on {}", bind_addr);

    let mut recv_buf = vec![0u8; 1500];

    loop {
        let (len, src_addr) = match socket.recv_from(&mut recv_buf).await {
            Ok(result) => result,
            Err(e) => {
                debug!("UDP recv error: {}", e);
                continue;
            }
        };

        // Rate limit check
        if !rate_limiter.allow(src_addr) {
            trace!("Rate limited packet from {}", src_addr);
            crate::server::metrics::metrics().total_voice_packets_dropped.inc();
            continue;
        }

        crate::server::metrics::metrics().total_voice_packets.inc();

        // Parse packet
        let packet = match VoicePacket::parse(&recv_buf[..len]) {
            Some(p) => p,
            None => {
                trace!("Invalid voice packet from {}", src_addr);
                continue;
            }
        };

        let sender_id = packet.user_id as i32;
        let channel_id = packet.channel_id as i32;

        // Sender must be joined to the channel they address.
        // Without this, any authenticated user could talk into any channel id.
        if !channel_manager.is_member(channel_id, sender_id) {
            trace!("User {} is not in channel {} — dropping", sender_id, channel_id);
            crate::server::metrics::metrics().total_voice_packets_dropped.inc();
            continue;
        }

        // Decrypt (authenticates the sender: only they hold their SRTP key)
        let plaintext = match srtp_manager.decrypt(sender_id, &packet.payload, packet.sequence) {
            Ok(p) => p,
            Err(_) => {
                trace!("SRTP decryption failed for user {} — rejecting", sender_id);
                continue;
            }
        };

        // Learn sender's UDP address only after the packet authenticated,
        // so a forged user_id cannot redirect another user's incoming audio.
        connection_manager.set_udp_addr(sender_id, src_addr);

        // Empty payload = client keepalive: address registered above, nothing to forward.
        // (Accepted from listen-only members too, or they would never receive audio.)
        if plaintext.is_empty() {
            continue;
        }

        // Real audio: the sender must be allowed to speak here (channel ACL / op net access).
        if !channel_manager.can_speak(channel_id, sender_id) {
            trace!("User {} is listen-only on channel {} — dropping", sender_id, channel_id);
            crate::server::metrics::metrics().total_voice_packets_dropped.inc();
            continue;
        }

        // Get channel members (excluding sender)
        let members = channel_manager.members(channel_id);
        let recipients: Vec<i32> = members.into_iter().filter(|&id| id != sender_id).collect();

        if recipients.is_empty() {
            continue;
        }

        // Parallel re-encryption and forwarding
        let socket = socket.clone();
        let srtp = srtp_manager.clone();
        let conns = connection_manager.clone();
        let pool = buffer_pool.clone();

        tokio::spawn(async move {
            let header = packet.build_forward_header();

            let mut tasks = Vec::with_capacity(recipients.len());

            for recipient_id in recipients {
                let srtp = srtp.clone();
                let conns = conns.clone();
                let socket = socket.clone();
                let pool = pool.clone();
                let plaintext = plaintext.clone();
                let header = header;

                tasks.push(tokio::spawn(async move {
                    // Get recipient's SRTP session for encryption
                    let encrypt_session = match srtp.get_encrypt_session(recipient_id) {
                        Some(s) => s,
                        None => return, // No SRTP session — skip
                    };

                    // Get recipient's UDP address
                    let dest_addr = match conns.get(recipient_id) {
                        Some(h) => match h.udp_addr {
                            Some(addr) => addr,
                            None => return, // No UDP address known — skip
                        },
                        None => return,
                    };

                    // Re-encrypt for recipient
                    let encrypted = match encrypt_session.encrypt(&plaintext, packet.sequence, sender_id as u32) {
                        Ok(e) => e,
                        Err(_) => return,
                    };

                    // Build outbound packet
                    let mut buf = pool.get();
                    buf.extend_from_slice(&header);
                    buf.extend_from_slice(&encrypted);

                    // Send
                    let _ = socket.send_to(&buf, dest_addr).await;

                    pool.put(buf);
                }));
            }

            // Wait for all sends to complete
            for task in tasks {
                let _ = task.await;
            }
        });
    }
}
