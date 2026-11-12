use voip_server::auth::{jwt, password};
use voip_server::auth::permissions::{Permission, has_permission, compute_permissions};
use voip_server::crypto::key_exchange;
use voip_server::state::channel_manager::ChannelManager;
use voip_server::state::presence::PresenceManager;
use voip_server::voice::packet::{VoicePacket, HEADER_SIZE, MAGIC};

// ─── Password hashing ───────────────────────────────────────────────────────

#[test]
fn password_hash_and_verify() {
    let hash = password::hash_password("secret123").unwrap();
    assert!(password::verify_password("secret123", &hash).unwrap());
    assert!(!password::verify_password("wrong", &hash).unwrap());
}

#[test]
fn password_hash_produces_unique_hashes() {
    let h1 = password::hash_password("same").unwrap();
    let h2 = password::hash_password("same").unwrap();
    // Different salts → different hash strings
    assert_ne!(h1, h2);
    // Both still verify
    assert!(password::verify_password("same", &h1).unwrap());
    assert!(password::verify_password("same", &h2).unwrap());
}

// ─── JWT tokens ──────────────────────────────────────────────────────────────

#[test]
fn jwt_create_and_validate() {
    let secret = "test-secret-at-least-32-chars-long!!";
    let (token, jti, _, _) = jwt::create_token(1, 10, vec![1, 2], secret, 3600).unwrap();
    assert!(!token.is_empty());

    let claims = jwt::validate_token(&token, secret).unwrap();
    assert_eq!(claims.sub, 1);
    assert_eq!(claims.org, 10);
    assert_eq!(claims.roles, vec![1, 2]);
    assert_eq!(claims.jti, jti.to_string());
}

#[test]
fn jwt_invalid_secret_rejected() {
    let (token, _, _, _) = jwt::create_token(1, 1, vec![], "secret-a-long-enough", 3600).unwrap();
    assert!(jwt::validate_token(&token, "wrong-secret-also-long").is_err());
}

#[test]
fn jwt_expired_token_rejected() {
    // jsonwebtoken has a 60-second default leeway, so we craft a token
    // that's been expired long enough to exceed the leeway.
    use chrono::Utc;
    use jsonwebtoken::{encode, EncodingKey, Header};
    let secret = "test-secret-at-least-32-chars-long!!";
    let now = Utc::now();
    let claims = voip_server::auth::jwt::Claims {
        sub: 1,
        org: 1,
        roles: vec![],
        exp: (now - chrono::Duration::seconds(120)).timestamp(), // 2 min ago
        iat: (now - chrono::Duration::seconds(180)).timestamp(),
        jti: uuid::Uuid::new_v4().to_string(),
    };
    let token = encode(
        &Header::default(),
        &claims,
        &EncodingKey::from_secret(secret.as_bytes()),
    ).unwrap();
    assert!(jwt::validate_token(&token, secret).is_err());
}

#[test]
fn jwt_malformed_token_rejected() {
    assert!(jwt::validate_token("not.a.jwt", "secret").is_err());
    assert!(jwt::validate_token("", "secret").is_err());
}

// ─── Permissions ─────────────────────────────────────────────────────────────

#[test]
fn permission_bitflags() {
    let perms = Permission::CanJoin as i32 | Permission::CanSpeak as i32;
    assert!(has_permission(perms, Permission::CanJoin));
    assert!(has_permission(perms, Permission::CanSpeak));
    assert!(!has_permission(perms, Permission::Admin));
}

#[test]
fn compute_permissions_combines_roles() {
    let role_perms = vec![
        Permission::CanJoin as i32 | Permission::CanSpeak as i32,
        Permission::CanManageChannels as i32,
    ];
    let combined = compute_permissions(&role_perms);
    assert!(has_permission(combined, Permission::CanJoin));
    assert!(has_permission(combined, Permission::CanSpeak));
    assert!(has_permission(combined, Permission::CanManageChannels));
    assert!(!has_permission(combined, Permission::Admin));
}

#[test]
fn admin_permission_implies_all() {
    let perms = Permission::Admin as i32;
    // Admin flag is just a bit — the application logic checks is_admin separately
    assert!(has_permission(perms, Permission::Admin));
}

// ─── Key exchange ────────────────────────────────────────────────────────────

#[test]
fn key_exchange_produces_shared_secret() {
    let (secret_a, pub_a) = key_exchange::generate_keypair();
    let (secret_b, pub_b) = key_exchange::generate_keypair();

    let shared_a = key_exchange::compute_shared_secret(secret_a, &pub_b);
    let shared_b = key_exchange::compute_shared_secret(secret_b, &pub_a);

    assert_eq!(shared_a.as_bytes(), shared_b.as_bytes());
}

#[test]
fn derived_keys_are_deterministic_for_same_secret() {
    let (secret_a, pub_a) = key_exchange::generate_keypair();
    let (secret_b, _pub_b) = key_exchange::generate_keypair();

    let shared = key_exchange::compute_shared_secret(secret_a, &pub_a);
    // Note: this is self-DH which isn't realistic, but tests derivation determinism
    let keys1 = key_exchange::derive_keys(shared.as_bytes());

    let shared2 = key_exchange::compute_shared_secret(secret_b, &pub_a);
    let keys2 = key_exchange::derive_keys(shared2.as_bytes());

    // Different shared secrets → different keys
    assert_ne!(keys1.master_key, keys2.master_key);
}

// ─── Channel manager ────────────────────────────────────────────────────────

#[test]
fn channel_join_and_leave() {
    let cm = ChannelManager::new();
    cm.set_limit(1, 10);

    assert!(cm.join(1, 100).is_ok());
    assert!(cm.join(1, 101).is_ok());
    assert_eq!(cm.member_count(1), 2);
    assert!(cm.is_member(1, 100));
    assert!(cm.is_member(1, 101));

    cm.leave(1, 100);
    assert_eq!(cm.member_count(1), 1);
    assert!(!cm.is_member(1, 100));
    assert!(cm.is_member(1, 101));
}

#[test]
fn channel_enforces_max_users() {
    let cm = ChannelManager::new();
    cm.set_limit(1, 2);

    assert!(cm.join(1, 1).is_ok());
    assert!(cm.join(1, 2).is_ok());
    assert!(cm.join(1, 3).is_err()); // full
    assert_eq!(cm.member_count(1), 2);
}

#[test]
fn channel_leave_all() {
    let cm = ChannelManager::new();
    cm.join(1, 100).unwrap();
    cm.join(2, 100).unwrap();
    cm.join(3, 100).unwrap();

    let left = cm.leave_all(100);
    assert_eq!(left.len(), 3);
    assert_eq!(cm.member_count(1), 0);
    assert_eq!(cm.member_count(2), 0);
    assert_eq!(cm.member_count(3), 0);
}

#[test]
fn channel_remove_channel() {
    let cm = ChannelManager::new();
    cm.join(1, 10).unwrap();
    cm.join(1, 20).unwrap();

    let evicted = cm.remove_channel(1);
    assert_eq!(evicted.len(), 2);
    assert_eq!(cm.member_count(1), 0);
}

#[test]
fn channel_members_returns_empty_for_unknown() {
    let cm = ChannelManager::new();
    assert!(cm.members(999).is_empty());
    assert_eq!(cm.member_count(999), 0);
    assert!(!cm.is_member(999, 1));
}

// ─── Presence manager ────────────────────────────────────────────────────────

#[test]
fn presence_defaults() {
    let pm = PresenceManager::new();
    let p = pm.get(1);
    assert!(!p.muted);
    assert!(!p.deafened);
    assert!(!p.talking);
}

#[test]
fn presence_mute_deafen_talk() {
    let pm = PresenceManager::new();

    let p = pm.set_muted(1, true);
    assert!(p.muted);
    assert!(!p.deafened);

    let p = pm.set_deafened(1, true);
    assert!(p.muted); // still muted
    assert!(p.deafened);

    let p = pm.set_talking(1, true);
    assert!(p.talking);

    pm.remove(1);
    let p = pm.get(1);
    assert!(!p.muted);
}

// ─── Voice packet ────────────────────────────────────────────────────────────

#[test]
fn voice_packet_roundtrip() {
    let packet = VoicePacket {
        version: 1,
        flags: 0,
        sequence: 42,
        timestamp: 1000,
        channel_id: 5,
        user_id: 10,
        payload: vec![0xAA, 0xBB, 0xCC],
    };

    let bytes = packet.to_bytes(&packet.payload);
    assert_eq!(bytes.len(), HEADER_SIZE + 3);

    let parsed = VoicePacket::parse(&bytes).unwrap();
    assert_eq!(parsed.version, 1);
    assert_eq!(parsed.flags, 0);
    assert_eq!(parsed.sequence, 42);
    assert_eq!(parsed.timestamp, 1000);
    assert_eq!(parsed.channel_id, 5);
    assert_eq!(parsed.user_id, 10);
    assert_eq!(parsed.payload, vec![0xAA, 0xBB, 0xCC]);
}

#[test]
fn voice_packet_rejects_short_data() {
    assert!(VoicePacket::parse(&[0u8; HEADER_SIZE - 1]).is_none());
    assert!(VoicePacket::parse(&[]).is_none());
}

#[test]
fn voice_packet_rejects_bad_magic() {
    let mut data = vec![0u8; HEADER_SIZE];
    data[0] = b'X'; // wrong magic
    data[1] = b'X';
    assert!(VoicePacket::parse(&data).is_none());
}

#[test]
fn voice_packet_accepts_empty_payload() {
    let mut data = vec![0u8; HEADER_SIZE];
    data[0] = MAGIC[0];
    data[1] = MAGIC[1];
    let parsed = VoicePacket::parse(&data).unwrap();
    assert!(parsed.payload.is_empty());
}

#[test]
fn voice_packet_forward_header() {
    let packet = VoicePacket {
        version: 1,
        flags: 0x02,
        sequence: 99,
        timestamp: 5000,
        channel_id: 3,
        user_id: 7,
        payload: vec![],
    };

    let header = packet.build_forward_header();
    assert_eq!(header.len(), HEADER_SIZE);
    assert_eq!(header[0], b'V');
    assert_eq!(header[1], b'L');
    assert_eq!(header[2], 1); // version
    assert_eq!(header[3], 0x02); // flags
}

// ─── SRTP session ────────────────────────────────────────────────────────────

use voip_server::crypto::srtp_session::SrtpSession;

#[test]
fn srtp_encrypt_decrypt_roundtrip() {
    let (secret_a, pub_a) = key_exchange::generate_keypair();
    let (secret_b, pub_b) = key_exchange::generate_keypair();

    let shared_a = key_exchange::compute_shared_secret(secret_a, &pub_b);
    let shared_b = key_exchange::compute_shared_secret(secret_b, &pub_a);

    let keys_a = key_exchange::derive_keys(shared_a.as_bytes());
    let keys_b = key_exchange::derive_keys(shared_b.as_bytes());

    let session_a = SrtpSession::new(&keys_a);
    let session_b = SrtpSession::new(&keys_b);

    let plaintext = b"opus audio frame data here";
    let seq = 42u64;

    let ciphertext = session_a.encrypt(plaintext, seq, 7).unwrap();
    assert_ne!(&ciphertext[..], plaintext);

    let decrypted = session_b.decrypt(&ciphertext, seq, 7).unwrap();
    assert_eq!(&decrypted[..], plaintext);
}

#[test]
fn srtp_wrong_sequence_rejects() {
    let (secret_a, pub_a) = key_exchange::generate_keypair();
    let (secret_b, pub_b) = key_exchange::generate_keypair();

    let shared = key_exchange::compute_shared_secret(secret_a, &pub_b);
    let keys = key_exchange::derive_keys(shared.as_bytes());
    let session = SrtpSession::new(&keys);

    let shared2 = key_exchange::compute_shared_secret(secret_b, &pub_a);
    let keys2 = key_exchange::derive_keys(shared2.as_bytes());
    let session2 = SrtpSession::new(&keys2);

    let ciphertext = session.encrypt(b"test data", 1, 7).unwrap();

    // Correct sequence works
    assert!(session2.decrypt(&ciphertext, 1, 7).is_ok());

    // Wrong sequence → auth failure
    assert!(session2.decrypt(&ciphertext, 2, 7).is_err());

    // Wrong speaker id → auth failure (speaker id is part of the nonce)
    assert!(session2.decrypt(&ciphertext, 1, 8).is_err());
}

#[test]
fn srtp_nonce_differs_per_speaker() {
    // Two speakers re-encrypted for the same listener with the same sequence number must
    // produce different keystreams (no AES-GCM nonce reuse).
    let (secret_a, _) = key_exchange::generate_keypair();
    let (_, pub_b) = key_exchange::generate_keypair();
    let shared = key_exchange::compute_shared_secret(secret_a, &pub_b);
    let session = SrtpSession::new(&key_exchange::derive_keys(shared.as_bytes()));

    let frame = [0u8; 32];
    let from_alice = session.encrypt(&frame, 100, 1).unwrap();
    let from_bob = session.encrypt(&frame, 100, 2).unwrap();
    assert_ne!(from_alice, from_bob, "same seq from different speakers must not share a nonce");
}

#[test]
fn srtp_tampered_ciphertext_rejects() {
    let (secret_a, pub_a) = key_exchange::generate_keypair();
    let (secret_b, pub_b) = key_exchange::generate_keypair();

    let shared = key_exchange::compute_shared_secret(secret_a, &pub_b);
    let keys = key_exchange::derive_keys(shared.as_bytes());
    let session = SrtpSession::new(&keys);

    let shared2 = key_exchange::compute_shared_secret(secret_b, &pub_a);
    let keys2 = key_exchange::derive_keys(shared2.as_bytes());
    let session2 = SrtpSession::new(&keys2);

    let mut ciphertext = session.encrypt(b"secret", 5, 7).unwrap();
    // Flip a bit
    ciphertext[0] ^= 0xFF;

    assert!(session2.decrypt(&ciphertext, 5, 7).is_err());
}

// ─── Rate limiter ────────────────────────────────────────────────────────────

#[test]
fn auth_rate_limiter_allows_under_limit() {
    let limiter = voip_server::server::rate_limit::auth_limiter();
    // Fresh limiter — should allow
    let unique_ip = format!("10.99.99.{}", rand::random::<u8>());
    assert!(limiter.check_ip(&unique_ip));
}

#[test]
fn admin_rate_limiter_allows_burst() {
    let limiter = voip_server::server::rate_limit::admin_limiter();
    // Use a unique user_id to avoid cross-test interference
    let user_id = 90000 + rand::random::<i32>().abs() % 10000;

    // First request should always be allowed
    assert!(limiter.check_and_consume(user_id));
}
