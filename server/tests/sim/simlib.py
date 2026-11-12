"""DadLink protocol simulator: real WebSocket control + X25519/HKDF/AES-GCM voice over UDP.

Mirrors the client exactly:
  - key exchange: X25519, HKDF-SHA256(salt="DadLink-SRTP-v2"), info "srtp-master-key"(16) / "srtp-master-salt"(14)
  - nonce: salt[:12] XOR (source_user_id u32 BE || sequence u64 BE)
  - packet: "VL" | ver u8 | flags u8 | seq u64 | ts u64 | channel u32 | user u32 | AES-128-GCM(payload)
"""
from __future__ import annotations

import asyncio
import base64
import json
import os
import struct
import time
from dataclasses import dataclass, field

import websockets
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.hashes import SHA256
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import serialization

HEADER = struct.Struct(">2sBBQQII")  # 28 bytes
MAGIC = b"VL"


def derive_keys(shared: bytes) -> tuple[bytes, bytes]:
    key = HKDF(algorithm=SHA256(), length=16, salt=b"DadLink-SRTP-v2", info=b"srtp-master-key").derive(shared)
    salt = HKDF(algorithm=SHA256(), length=14, salt=b"DadLink-SRTP-v2", info=b"srtp-master-salt").derive(shared)
    return key, salt


def nonce(salt: bytes, seq: int, source_id: int) -> bytes:
    raw = struct.pack(">IQ", source_id & 0xFFFFFFFF, seq)
    return bytes(a ^ b for a, b in zip(raw, salt[:12]))


class Srtp:
    def __init__(self, key: bytes, salt: bytes):
        self.aead = AESGCM(key)
        self.salt = salt

    def encrypt(self, pt: bytes, seq: int, source_id: int) -> bytes:
        return self.aead.encrypt(nonce(self.salt, seq, source_id), pt, None)

    def decrypt(self, ct: bytes, seq: int, source_id: int) -> bytes:
        return self.aead.decrypt(nonce(self.salt, seq, source_id), ct, None)


@dataclass
class Rx:
    channel: int
    sender: int
    seq: int
    sent_at: float
    recv_at: float
    ok: bool


class _Udp(asyncio.DatagramProtocol):
    def __init__(self, client: "SimClient"):
        self.client = client

    def datagram_received(self, data, addr):
        self.client._on_udp(data)


@dataclass
class SimClient:
    host: str
    port: int
    voice_port: int
    org: str
    name: str = ""
    user_id: int = 0
    token: str = ""
    auth: dict = field(default_factory=dict)
    inbox: list = field(default_factory=list)
    rx: list = field(default_factory=list)
    decrypt_failures: int = 0
    can_speak: dict = field(default_factory=dict)
    # Capacity mode: fully decrypt only every Nth packet (others are counted from the header,
    # latency from the header timestamp) so the Python simulator isn't the bottleneck.
    sample: int = 1

    def __post_init__(self):
        self.ws = None
        self.srtp: Srtp | None = None
        self.prev_srtp: Srtp | None = None
        self.seq = 0
        self.transport = None
        self.joined: set[int] = set()
        self._events: dict[str, asyncio.Event] = {}
        self._reader: asyncio.Task | None = None
        self.key_exchanges = 0

    # ── control ──
    async def connect(self):
        self.ws = await websockets.connect(f"ws://{self.host}:{self.port}/ws", max_size=None, ping_interval=None)
        self._reader = asyncio.create_task(self._read())
        loop = asyncio.get_running_loop()
        if self.transport is None:
            self.transport, _ = await loop.create_datagram_endpoint(lambda: _Udp(self), local_addr=("127.0.0.1", 0))
            import socket
            try:
                self.transport.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
            except OSError:
                pass

    async def _read(self):
        try:
            async for raw in self.ws:
                m = json.loads(raw)
                t = m.get("type")
                if t == "key_exchange_init":
                    await self._key_exchange(m["payload"]["public_key"])
                    continue
                if t == "channel_joined":
                    p = m["payload"]
                    self.joined.add(p["channel_id"])
                    self.can_speak[p["channel_id"]] = p.get("can_speak", True)
                self.inbox.append(m)
                ev = self._events.get(t)
                if ev:
                    ev.set()
        except (websockets.ConnectionClosed, asyncio.CancelledError):
            pass

    async def _key_exchange(self, server_pub_b64: str):
        priv = X25519PrivateKey.generate()
        shared = priv.exchange(X25519PublicKey.from_public_bytes(base64.b64decode(server_pub_b64)))
        key, salt = derive_keys(shared)
        self.prev_srtp, self.srtp = self.srtp, Srtp(key, salt)
        self.key_exchanges += 1
        pub = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        await self.send("key_exchange_response", {"public_key": base64.b64encode(pub).decode()})

    async def send(self, t: str, payload: dict):
        await self.ws.send(json.dumps({"type": t, "payload": payload}))

    async def wait_for(self, t: str, timeout=5.0, pred=None):
        deadline = time.monotonic() + timeout
        while True:
            for i, m in enumerate(self.inbox):
                if m["type"] == t and (pred is None or pred(m["payload"])):
                    return self.inbox.pop(i)["payload"]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ev = self._events.setdefault(t, asyncio.Event())
            ev.clear()
            try:
                await asyncio.wait_for(ev.wait(), remaining)
            except asyncio.TimeoutError:
                pass

    def take(self, t: str) -> list:
        out = [m["payload"] for m in self.inbox if m["type"] == t]
        self.inbox = [m for m in self.inbox if m["type"] != t]
        return out

    async def login_password(self, username: str, password: str) -> bool:
        await self.connect()
        await self.send("authenticate", {"method": "password", "username": username, "password": password, "org_tag": self.org})
        return await self._await_auth()

    async def login_token(self, token: str) -> bool:
        await self.connect()
        await self.send("authenticate", {"method": "token", "token": token, "org_tag": self.org})
        return await self._await_auth()

    async def login_discord(self, ticket: str, org: str | None = None) -> bool:
        await self.connect()
        await self.send("authenticate", {"method": "discord", "token": ticket, "org_tag": org or self.org})
        return await self._await_auth()

    async def _await_auth(self, timeout=30.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for i, m in enumerate(self.inbox):
                if m["type"] == "auth_success":
                    self.auth = self.inbox.pop(i)["payload"]
                    self.user_id, self.token = self.auth["user_id"], self.auth["token"]
                    return True
                if m["type"] in ("auth_failed", "error"):
                    self.auth = self.inbox.pop(i)["payload"]
                    return False
            await asyncio.sleep(0.02)
        return False

    async def join(self, channel_id: int, timeout=5.0):
        await self.send("join_channel", {"channel_id": channel_id})
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for i, m in enumerate(self.inbox):
                if m["type"] == "channel_joined" and m["payload"]["channel_id"] == channel_id:
                    return self.inbox.pop(i)["payload"], None
                if m["type"] == "error":
                    return None, self.inbox.pop(i)["payload"]
            await asyncio.sleep(0.02)
        return None, {"code": "timeout"}

    async def leave(self, channel_id: int):
        await self.send("leave_channel", {"channel_id": channel_id})
        self.joined.discard(channel_id)

    async def wait_keys(self, timeout=5.0) -> bool:
        deadline = time.monotonic() + timeout
        while self.srtp is None and time.monotonic() < deadline:
            await asyncio.sleep(0.02)
        # give the server a moment to install the key from our response
        await asyncio.sleep(0.15)
        return self.srtp is not None

    async def close(self):
        if self.ws:
            await self.ws.close()
        if self._reader:
            self._reader.cancel()
        if self.transport:
            self.transport.close()
            self.transport = None

    # ── voice ──
    def send_voice(self, channel_id: int, payload: bytes | None = None, *, as_user: int | None = None,
                   seq: int | None = None, size: int = 60):
        """Send one frame. Payload carries send time + our id + seq for latency/integrity checks."""
        if self.srtp is None:
            return
        s = self.seq if seq is None else seq
        if seq is None:
            self.seq += 1
        uid = self.user_id if as_user is None else as_user
        if payload is None:
            body = struct.pack(">dII", time.time(), self.user_id, s)
            payload = body + b"\x00" * max(0, size - len(body))
        ct = self.srtp.encrypt(payload, s, self.user_id)  # always encrypted with OUR key/id
        pkt = HEADER.pack(MAGIC, 1, 0, s, int(time.time() * 1000), channel_id, uid) + ct
        self.transport.sendto(pkt, (self.host, self.voice_port))

    def send_keepalive(self, channel_id: int):
        if self.srtp is None:
            return
        s = self.seq
        self.seq += 1
        ct = self.srtp.encrypt(b"", s, self.user_id)
        self.transport.sendto(HEADER.pack(MAGIC, 1, 0, s, 0, channel_id, self.user_id) + ct, (self.host, self.voice_port))

    def _on_udp(self, data: bytes):
        if len(data) < HEADER.size:
            return
        magic, ver, flags, seq, ts, ch, uid = HEADER.unpack(data[:HEADER.size])
        ct = data[HEADER.size:]
        if self.sample > 1 and seq % self.sample:
            now = time.time()
            self.rx.append(Rx(ch, uid, seq, ts / 1000.0 if ts else 0.0, now, True))
            return
        pt = None
        for s in (self.srtp, self.prev_srtp):
            if s is None:
                continue
            try:
                pt = s.decrypt(ct, seq, uid)
                break
            except Exception:
                pass
        now = time.time()
        if pt is None:
            self.decrypt_failures += 1
            self.rx.append(Rx(ch, uid, seq, 0.0, now, False))
            return
        sent_at = struct.unpack(">d", pt[:8])[0] if len(pt) >= 8 else 0.0
        self.rx.append(Rx(ch, uid, seq, sent_at, now, True))
