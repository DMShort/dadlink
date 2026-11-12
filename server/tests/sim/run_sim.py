#!/usr/bin/env python3
"""DadLink simulation suite.

Starts a fresh server (release build) against Postgres with a mock Discord, then runs
protocol-level scenarios with simulated clients. Prints a pass rate and writes
results.json + report.md next to this file.

  python3 run_sim.py [--users 100] [--speakers 12] [--seconds 5] [--target 0.95]
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import os
import random
import secrets
import shutil
import statistics
import subprocess
import sys
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlparse

import requests

sys.path.insert(0, str(Path(__file__).parent))
from simlib import Srtp, SimClient, derive_keys  # noqa: E402

HERE = Path(__file__).resolve().parent
SERVER_DIR = HERE.parent.parent
CLIENT_SRC = SERVER_DIR.parent / "client" / "src"
HOST, PORT, VPORT, MOCK_PORT = "127.0.0.1", 9100, 9101, 9900
BASE = f"http://{HOST}:{PORT}"
ORG = "scda"
ADMIN_PW = "SimAdmin#2026"
GUILD = "900000000000000001"
DROLE_PILOT, DROLE_CMD, DROLE_GONE = "800000000000000011", "800000000000000012", "800000000000000013"

# ─────────────────────────── mock Discord ───────────────────────────

class MockDiscord:
    def __init__(self):
        self.users: dict[str, dict] = {}       # discord id -> {username, global_name, guild, nick, roles}
        self.codes: dict[str, str] = {}        # code -> discord id
        self.next_user: str | None = None
        self.deny_next = False
        self.fail_token = False
        self.lock = threading.Lock()

    def add(self, did, username, *, global_name=None, guild=True, nick=None, roles=()):
        self.users[did] = dict(username=username, global_name=global_name, guild=guild, nick=nick, roles=list(roles))


MOCK = MockDiscord()


class MockHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _user(self):
        auth = self.headers.get("Authorization", "")
        code = auth.removeprefix("Bearer at_")
        return MOCK.users.get(MOCK.codes.get(code, ""))

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/oauth2/authorize":
            with MOCK.lock:
                if MOCK.deny_next:
                    MOCK.deny_next = False
                    loc = f"{q['redirect_uri']}?{urlencode({'error': 'access_denied', 'state': q['state']})}"
                else:
                    code = secrets.token_hex(8)
                    MOCK.codes[code] = MOCK.next_user
                    loc = f"{q['redirect_uri']}?{urlencode({'code': code, 'state': q['state']})}"
            self.send_response(302)
            self.send_header("Location", loc)
            self.end_headers()
            return
        if u.path == "/api/users/@me":
            user = self._user()
            if not user:
                return self._json(401, {"message": "401: Unauthorized"})
            did = next(k for k, v in MOCK.users.items() if v is user)
            return self._json(200, {"id": did, "username": user["username"], "global_name": user["global_name"]})
        if u.path == f"/api/users/@me/guilds/{GUILD}/member":
            user = self._user()
            if not user:
                return self._json(401, {})
            if not user["guild"]:
                return self._json(404, {"message": "Unknown Guild", "code": 10004})
            return self._json(200, {"nick": user["nick"], "roles": user["roles"]})
        self._json(404, {})

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/api/oauth2/token":
            n = int(self.headers.get("Content-Length", 0))
            form = {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode()).items()}
            if MOCK.fail_token:
                return self._json(500, {"message": "boom"})
            if form.get("code") not in MOCK.codes or form.get("client_secret") != "sim-secret":
                return self._json(400, {"error": "invalid_grant"})
            return self._json(200, {"access_token": "at_" + form["code"], "token_type": "Bearer"})
        self._json(404, {})


def discord_login(did: str | None, *, deny=False, verifier=None, poll_with=None, org=ORG):
    """Simulate the whole browser + poll flow. Returns (poll_json, callback_status)."""
    verifier = verifier or secrets.token_hex(32)
    challenge = hashlib.sha256(verifier.encode()).hexdigest()
    with MOCK.lock:
        MOCK.next_user = did
        MOCK.deny_next = deny
    r = requests.get(f"{BASE}/auth/discord/start", params={"org": org, "challenge": challenge}, allow_redirects=True, timeout=10)
    p = requests.get(f"{BASE}/auth/discord/poll", params={"verifier": poll_with or verifier}, timeout=10).json()
    return p, r.status_code


# ─────────────────────────── harness ───────────────────────────

RESULTS: list[dict] = []
STATE: dict = {}


def record(name, group, ok, detail="", ms=0.0):
    RESULTS.append(dict(name=name, group=group, ok=bool(ok), detail=str(detail), ms=round(ms, 1)))
    mark = "PASS" if ok else "FAIL"
    print(f"  [{mark}] {group:<10} {name:<52} {detail}")


async def run_test(group, name, fn, timeout=120):
    t0 = time.perf_counter()
    try:
        ok, detail = await asyncio.wait_for(fn(), timeout)
    except Exception as e:  # a crash is a failure, not an abort
        ok, detail = False, f"exception: {e!r}"
        traceback.print_exc()
    record(name, group, ok, detail, (time.perf_counter() - t0) * 1000)


def api(token, method, path, body=None):
    r = requests.request(method, BASE + path, headers={"Authorization": f"Bearer {token}"}, json=body, timeout=30)
    try:
        return r.status_code, r.json()
    except Exception:
        return r.status_code, {}


def new_client(name=""):
    return SimClient(HOST, PORT, VPORT, ORG, name=name)


async def logged_in(username, password="Passw0rd!x"):
    c = new_client(username)
    ok = await c.login_password(username, password)
    if not ok:
        raise RuntimeError(f"login failed for {username}: {c.auth}")
    return c


async def drain(seconds):
    await asyncio.sleep(seconds)


# ─────────────────────────── scenarios ───────────────────────────

async def t_interop_cpp_to_py():
    exe = STATE.get("interop")
    if not exe:
        return False, "interop harness not built"
    from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
    from cryptography.hazmat.primitives import serialization
    import base64
    priv = X25519PrivateKey.generate()
    pub = base64.b64encode(priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)).decode()
    out = subprocess.run([exe, "derive", pub], capture_output=True, text=True, timeout=10).stdout.split()
    cpub, key_hex, salt_hex, ct_hex = out
    key, salt = derive_keys(priv.exchange(X25519PublicKey.from_public_bytes(base64.b64decode(cpub))))
    same_keys = key.hex() == key_hex and salt.hex() == salt_hex
    pt = Srtp(key, salt).decrypt(bytes.fromhex(ct_hex), 777, 42)
    STATE["interop_keys"] = (key, salt)
    return same_keys and pt == b"dadlink-interop", f"keys match={same_keys}, plaintext={pt!r}"


async def t_interop_py_to_cpp():
    exe = STATE.get("interop")
    key, salt = STATE.get("interop_keys", (os.urandom(16), os.urandom(14)))
    ct = Srtp(key, salt).encrypt(b"from-python", 31337, 9)
    out = subprocess.run([exe, "decrypt", key.hex(), salt.hex(), ct.hex(), "31337", "9"], capture_output=True, text=True, timeout=10).stdout.strip()
    wrong = subprocess.run([exe, "decrypt", key.hex(), salt.hex(), ct.hex(), "31337", "10"], capture_output=True, text=True, timeout=10).stdout.strip()
    return out == "from-python" and wrong == "FAIL", f"decrypt={out!r}, wrong speaker id -> {wrong}"


async def setup_small():
    """Four users in General plus a private channel. Returns dict of clients."""
    admin = STATE["admin"]
    cs = {n: await logged_in(n) for n in ("alice", "bob", "carol", "dave")}
    general = STATE["general"]
    for c in cs.values():
        j, e = await c.join(general)
        assert j, e
        assert await c.wait_keys()
        c.send_keepalive(general)  # the real client sends one immediately, then every 5 s
    await asyncio.sleep(0.2)
    return cs


async def t_basic_delivery():
    cs = STATE["small"]
    a, b = cs["alice"], cs["bob"]
    for c in cs.values():
        c.rx.clear()
    for _ in range(50):
        a.send_voice(STATE["general"])
        await asyncio.sleep(0.02)
    await drain(0.4)
    got = [r for r in b.rx if r.sender == a.user_id and r.ok]
    return len(got) >= 50, f"bob received {len(got)}/50 from alice, decrypt failures={b.decrypt_failures}"


async def t_listener_without_talking():
    # dave has never transmitted; the keepalive alone must register his address
    cs = STATE["small"]
    d = cs["dave"]
    d.send_keepalive(STATE["general"])
    await drain(0.2)
    d.rx.clear()
    for _ in range(20):
        cs["alice"].send_voice(STATE["general"])
        await asyncio.sleep(0.02)
    await drain(0.4)
    got = sum(1 for r in d.rx if r.ok)
    return got >= 20, f"silent listener received {got}/20"


async def t_no_echo():
    cs = STATE["small"]
    a = cs["alice"]
    a.rx.clear()
    for _ in range(20):
        a.send_voice(STATE["general"])
    await drain(0.4)
    own = [r for r in a.rx if r.sender == a.user_id]
    return len(own) == 0, f"echoed packets: {len(own)}"


async def t_non_member_dropped():
    cs = STATE["small"]
    private = STATE["private"]
    carol, bob = cs["carol"], cs["bob"]
    j, e = await bob.join(private)
    if not j:
        return False, f"bob could not join private: {e}"
    bob.rx.clear()
    for _ in range(20):
        carol.send_voice(private)  # carol is NOT in private
    await drain(0.4)
    leaked = [r for r in bob.rx if r.channel == private]
    await bob.leave(private)
    return len(leaked) == 0, f"packets leaked into channel carol isn't in: {len(leaked)}"


async def t_forged_sender_dropped():
    cs = STATE["small"]
    carol, bob, alice = cs["carol"], cs["bob"], cs["alice"]
    for c in cs.values():
        c.rx.clear()
    for _ in range(20):
        carol.send_voice(STATE["general"], as_user=alice.user_id)  # header claims alice
    await drain(0.4)
    forged = [r for r in bob.rx if r.sender == alice.user_id]
    # alice must still receive (her address not hijacked by carol's forged packets)
    alice.rx.clear()
    for _ in range(10):
        bob.send_voice(STATE["general"])
    await drain(0.4)
    alice_ok = sum(1 for r in alice.rx if r.sender == bob.user_id and r.ok)
    return len(forged) == 0 and alice_ok >= 10, f"forged delivered={len(forged)}, alice still receives {alice_ok}/10"


async def t_simultaneous_speakers_same_seq():
    cs = STATE["small"]
    a, b, d = cs["alice"], cs["bob"], cs["dave"]
    for c in cs.values():
        c.rx.clear()
    base = 900000
    for i in range(10):
        a.send_voice(STATE["general"], seq=base + i)
        b.send_voice(STATE["general"], seq=base + i)
    await drain(0.4)
    from_a = sum(1 for r in d.rx if r.sender == a.user_id and r.ok and r.seq >= base)
    from_b = sum(1 for r in d.rx if r.sender == b.user_id and r.ok and r.seq >= base)
    return from_a == 10 and from_b == 10 and d.decrypt_failures == 0, \
        f"listener decrypted {from_a}/10 + {from_b}/10 with identical seqs, failures={d.decrypt_failures}"


async def t_multi_channel_tagging():
    cs = STATE["small"]
    a, b, c = cs["alice"], cs["bob"], cs["carol"]
    private = STATE["private"]
    for x in (a, c):
        j, e = await x.join(private)
        if not j:
            return False, f"join private failed: {e}"
    a.send_keepalive(private)
    for x in cs.values():
        x.rx.clear()
    for _ in range(10):
        b.send_voice(STATE["general"])
        c.send_voice(private)
    await drain(0.4)
    gen = sum(1 for r in a.rx if r.channel == STATE["general"] and r.sender == b.user_id)
    prv = sum(1 for r in a.rx if r.channel == private and r.sender == c.user_id)
    for x in (a, c):
        await x.leave(private)
    return gen == 10 and prv == 10, f"general {gen}/10, private {prv}/10 (correctly tagged)"


async def t_leave_stops_audio():
    cs = STATE["small"]
    a, b = cs["alice"], cs["bob"]
    await b.leave(STATE["general"])
    await drain(0.3)
    b.rx.clear()
    for _ in range(10):
        a.send_voice(STATE["general"])
    await drain(0.4)
    got = len(b.rx)
    j, _ = await b.join(STATE["general"])
    return got == 0 and j is not None, f"after leaving received {got}; rejoined={bool(j)}"


async def t_rapid_multi_join_keys():
    """Join several channels back-to-back (what 'Join op' does) and check voice still works."""
    admin = STATE["admin"]
    chans = []
    for i in range(3):
        s, b = api(admin.token, "POST", "/admin/channels", {"name": f"rapid-{i}-{secrets.token_hex(2)}"})
        chans.append(b["channel"]["id"])
    x = await logged_in("erin")
    y = await logged_in("frank")
    for c in chans:
        await y.join(c)
    await y.wait_keys()
    # erin fires all joins without waiting, like the client's joinOp()
    for c in chans:
        await x.send("join_channel", {"channel_id": c})
    await drain(1.0)
    x.send_keepalive(chans[0])
    await drain(0.2)
    x.rx.clear()
    for _ in range(10):
        y.send_voice(chans[0])
    await drain(0.4)
    got = sum(1 for r in x.rx if r.ok)
    errors = [m["payload"] for m in x.inbox if m["type"] == "error"]
    await x.close(); await y.close()
    return got >= 10 and x.decrypt_failures == 0, \
        f"received {got}/10, decrypt failures={x.decrypt_failures}, key exchanges={x.key_exchanges}, errors={errors[:2]}"


# ── Discord ──

async def t_discord_config():
    r = requests.get(f"{BASE}/auth/config", timeout=5).json()
    return r.get("discord") is True, str(r)


async def t_discord_new_user():
    MOCK.add("111", "viper", global_name="Viper One", nick="Viper-1", roles=[DROLE_PILOT])
    p, st = discord_login("111")
    if p.get("status") != "ok":
        return False, f"poll={p}, callback={st}"
    c = new_client("viper")
    ok = await c.login_discord(p["ticket"])
    STATE["viper"] = c
    return ok and c.user_id > 0, f"ws auth={ok}, user_id={c.user_id}, callback={st}"


async def t_discord_relogin_links():
    p, _ = discord_login("111")
    c = new_client("viper2")
    ok = await c.login_discord(p["ticket"])
    same = ok and c.user_id == STATE["viper"].user_id
    await c.close()
    return same, f"second login user_id={c.user_id}, first={STATE['viper'].user_id}"


async def t_discord_non_member():
    MOCK.add("222", "outsider", guild=False)
    p, st = discord_login("222")
    return p.get("status") == "error" and "member" in p.get("reason", ""), f"{p}, callback={st}"


async def t_discord_cancelled():
    p, st = discord_login("111", deny=True)
    return p.get("status") == "error" and "cancel" in p.get("reason", "").lower(), f"{p}"


async def t_discord_wrong_verifier():
    real = secrets.token_hex(32)
    p_bad, _ = discord_login("111", verifier=real, poll_with=secrets.token_hex(32))
    p_good = requests.get(f"{BASE}/auth/discord/poll", params={"verifier": real}, timeout=5).json()
    p_again = requests.get(f"{BASE}/auth/discord/poll", params={"verifier": real}, timeout=5).json()
    ok = p_bad.get("status") == "pending" and p_good.get("status") == "ok" and p_again.get("status") == "pending"
    return ok, f"wrong verifier={p_bad.get('status')}, right={p_good.get('status')}, second collect={p_again.get('status')}"


async def t_discord_ticket_single_use():
    p, _ = discord_login("111")
    c1, c2 = new_client(), new_client()
    ok1 = await c1.login_discord(p["ticket"])
    ok2 = await c2.login_discord(p["ticket"])
    await c1.close(); await c2.close()
    return ok1 and not ok2, f"first use={ok1}, reuse={ok2} ({c2.auth.get('reason')})"


async def t_discord_org_mismatch():
    p, _ = discord_login("111")
    c = new_client()
    ok = await c.login_discord(p["ticket"], org="other-org")
    await c.close()
    return not ok, f"ticket for scda used on other-org accepted={ok} ({c.auth.get('reason')})"


async def t_discord_username_collision():
    # 'alice' already exists as a password user
    MOCK.add("333", "alice", global_name="Discord Alice", roles=[])
    p, _ = discord_login("333")
    c = new_client()
    ok = await c.login_discord(p["ticket"])
    s, b = api(STATE["admin"].token, "GET", "/admin/users")
    names = [u["username"] for u in b["users"]]
    await c.close()
    return ok and "alice2" in names, f"created usernames include alice2={'alice2' in names}"


async def t_discord_api_failure():
    MOCK.fail_token = True
    try:
        p, st = discord_login("111")
    finally:
        MOCK.fail_token = False
    return p.get("status") == "error" and st == 403, f"{p}, callback={st}"


async def t_discord_bad_challenge():
    r = requests.get(f"{BASE}/auth/discord/start", params={"org": ORG, "challenge": "nothex"}, allow_redirects=False, timeout=5)
    r2 = requests.get(f"{BASE}/auth/discord/start", params={"org": "nope", "challenge": "a" * 64}, allow_redirects=False, timeout=5)
    return r.status_code == 400 and r2.status_code == 404, f"bad challenge={r.status_code}, unknown org={r2.status_code}"


async def t_discord_disabled_account():
    MOCK.add("444", "benched", roles=[])
    p, _ = discord_login("444")
    c = new_client()
    await c.login_discord(p["ticket"])
    uid = c.user_id
    await c.close()
    s, _ = api(STATE["admin"].token, "PUT", f"/admin/users/{uid}", {"is_active": False})
    p2, st = discord_login("444")
    return p2.get("status") == "error" and "disabled" in p2.get("reason", ""), f"admin disable={s}, then {p2}"


async def t_discord_role_sync():
    """Grant on login, then remove when the Discord role is taken away."""
    MOCK.add("555", "gunner", roles=[DROLE_CMD])
    p, _ = discord_login("555")
    c = new_client()
    await c.login_discord(p["ticket"])
    j1, _ = await c.join(STATE["cmd_only"])
    await c.close()
    MOCK.users["555"]["roles"] = []
    p2, _ = discord_login("555")
    c2 = new_client()
    await c2.login_discord(p2["ticket"])
    j2, e2 = await c2.join(STATE["cmd_only"])
    await c2.close()
    return j1 is not None and j2 is None, f"with Discord role: joined={j1 is not None}; after removal: joined={j2 is not None} ({(e2 or {}).get('code')})"


async def t_discord_display_name():
    c = STATE["viper"]
    s, b = api(STATE["admin"].token, "GET", "/admin/users")
    u = next(u for u in b["users"] if u["id"] == c.user_id)
    return u.get("display_name") == "Viper-1", f"display_name={u.get('display_name')!r} (guild nick)"


# ── Load / op ──

async def setup_load(n):
    admin = STATE["admin"]
    t0 = time.perf_counter()
    sem = asyncio.Semaphore(20)

    async def login(i):
        async with sem:
            return await logged_in(f"sim{i:03d}")
    clients = await asyncio.gather(*[login(i) for i in range(n)])
    for c in clients:
        c.sample = STATE.get("sample", 1)
    STATE["load_login_ms"] = (time.perf_counter() - t0) * 1000
    return clients


async def t_load_logins():
    n = STATE["n"]
    clients = await setup_load(n)
    STATE["load"] = clients
    ok = len(clients) == n and all(c.user_id for c in clients)
    return ok, f"{len(clients)}/{n} logged in, {STATE['load_login_ms']:.0f} ms total"


async def t_op_start_notifies_all():
    officer = STATE["officer"]
    clients = STATE["load"]
    for c in clients:
        c.take("op_started")
    t0 = time.time()
    s, b = api(officer.token, "POST", "/ops", {"template_id": STATE["template_id"], "name": f"LoadOp-{secrets.token_hex(2)}"})
    if s != 201:
        return False, f"start op failed {s} {b}"
    STATE["load_op"] = b["op"]
    got = await asyncio.gather(*[c.wait_for("op_started", 5) for c in clients])
    latency = (time.time() - t0) * 1000
    STATE["load_views"] = got
    n_ok = sum(1 for g in got if g)
    return n_ok == len(clients) and latency < 3000, f"{n_ok}/{len(clients)} notified in {latency:.0f} ms"


async def t_op_join_assigned():
    clients, views = STATE["load"], STATE["load_views"]
    sem = asyncio.Semaphore(25)
    talk = listen = fails = 0

    async def join_all(c, v):
        nonlocal talk, listen, fails
        async with sem:
            # Fire all joins at once like the client's joinOp(), then wait for the replies
            nets = [n for n in v["op"]["nets"] if n["assigned"]]
            for n in nets:
                await c.send("join_channel", {"channel_id": n["channel_id"]})
            for n in nets:
                j = await c.wait_for("channel_joined", 8, pred=lambda p, ch=n["channel_id"]: p["channel_id"] == ch)
                if not j:
                    fails += 1
                    continue
                c.joined.add(n["channel_id"])
                c.can_speak[n["channel_id"]] = j["can_speak"]
                expect = n["access"] == "talk"
                if j["can_speak"] != expect:
                    fails += 1
                talk += j["can_speak"]
                listen += not j["can_speak"]
            await c.wait_keys()
            for ch in c.joined:
                c.send_keepalive(ch)
    await asyncio.gather(*[join_all(c, v) for c, v in zip(clients, views)])
    return fails == 0, f"joins: {talk} talk + {listen} listen-only, mismatches/timeouts={fails}"


async def t_load_voice():
    """N speakers talk at 50 pps for S seconds on their talk nets; everyone else listens."""
    clients = STATE["load"]
    secs, n_speakers = STATE["seconds"], STATE["speakers"]
    nets = {n["name"]: n["channel_id"] for n in STATE["load_op"]["nets"]}
    for c in clients:
        c.rx.clear(); c.decrypt_failures = 0
    # pick speakers: Command first, then wings
    speakers = []
    by_net = {}
    for c in clients:
        for ch, ok in c.can_speak.items():
            if ok and ch in nets.values():
                by_net.setdefault(ch, []).append(c)
    order = [nets["Command"]] + [nets[k] for k in nets if k != "Command"]
    i = 0
    while len(speakers) < n_speakers and any(by_net.get(ch) for ch in order):
        ch = order[i % len(order)]
        if by_net.get(ch):
            speakers.append((by_net[ch].pop(0), ch))
        i += 1
    members = {ch: [c for c in clients if ch in c.joined] for ch in nets.values()}
    frames = int(secs * 50)

    async def talk(c, ch):
        start = time.perf_counter()
        for k in range(frames):
            c.send_voice(ch)
            delay = start + (k + 1) * 0.02 - time.perf_counter()
            if delay > 0:
                await asyncio.sleep(delay)

    def cpu():
        with open(f"/proc/{STATE['server_proc'].pid}/stat") as f:
            p = f.read().split()
        return (int(p[13]) + int(p[14])) / os.sysconf("SC_CLK_TCK")
    cpu0, t0 = cpu(), time.perf_counter()
    await asyncio.gather(*[talk(c, ch) for c, ch in speakers])
    talk_wall = time.perf_counter() - t0
    # wait until deliveries stop arriving (backlog drained), max 10 s
    last = -1
    for _ in range(50):
        await drain(0.2)
        now = sum(len(c.rx) for c in clients)
        if now == last:
            break
        last = now
    server_cpu = cpu() - cpu0
    STATE["voice_server_cores"] = server_cpu / max(talk_wall, 0.001)
    expected = sum(frames * (len(members[ch]) - 1) for c, ch in speakers)
    speaker_ids = {c.user_id for c, _ in speakers}
    delivered = sum(1 for c in clients for r in c.rx if r.ok and r.sender in speaker_ids)
    lat = sorted((r.recv_at - r.sent_at) * 1000 for c in clients for r in c.rx if r.ok and r.sent_at)
    fails = sum(c.decrypt_failures for c in clients)
    ratio = delivered / expected if expected else 0
    STATE["load_stats"] = dict(speakers=len(speakers), frames=frames, expected=expected, delivered=delivered,
                               ratio=ratio, p50=lat[len(lat) // 2] if lat else None,
                               p95=lat[int(len(lat) * 0.95)] if lat else None,
                               p99=lat[int(len(lat) * 0.99)] if lat else None,
                               max=lat[-1] if lat else None, decrypt_failures=fails,
                               wall_s=time.perf_counter() - t0, talk_s=talk_wall,
                               server_cpu_s=server_cpu, server_cores_busy=STATE["voice_server_cores"],
                               fwd_per_s=delivered / max(talk_wall, 0.001))
    return ratio >= 0.95, (f"{len(speakers)} speakers -> delivered {delivered}/{expected} ({ratio:.2%}); "
                           f"server CPU {STATE['voice_server_cores']:.2f} cores over {talk_wall:.1f} s send window")


async def t_load_latency():
    st = STATE.get("load_stats") or {}
    p95 = st.get("p95")
    return p95 is not None and p95 < 50, f"p50={st.get('p50', 0):.1f} ms, p95={p95 or 0:.1f} ms, p99={st.get('p99', 0):.1f} ms (loopback)"


async def t_load_integrity():
    st = STATE.get("load_stats") or {}
    return st.get("decrypt_failures", 1) == 0, f"decrypt failures across all listeners: {st.get('decrypt_failures')}"


async def t_listen_only_enforced_under_load():
    clients = STATE["load"]
    nets = {n["name"]: n["channel_id"] for n in STATE["load_op"]["nets"]}
    cmd = nets["Command"]
    lo = [c for c in clients if c.can_speak.get(cmd) is False][:3]
    if not lo:
        return False, "no listen-only members found"
    ids = {c.user_id for c in lo}
    for c in clients:
        c.rx.clear()
    for _ in range(50):
        for c in lo:
            c.send_voice(cmd)
        await asyncio.sleep(0.02)
    await drain(0.5)
    leaked = sum(1 for c in clients for r in c.rx if r.sender in ids and r.channel == cmd)
    return leaked == 0, f"{len(lo)} listen-only users x 50 frames on Command -> {leaked} delivered"


async def t_server_resources():
    pid = STATE["server_proc"].pid
    try:
        with open(f"/proc/{pid}/status") as f:
            rss = next(int(l.split()[1]) for l in f if l.startswith("VmRSS")) / 1024
        with open(f"/proc/{pid}/stat") as f:
            parts = f.read().split()
        cpu_s = (int(parts[13]) + int(parts[14])) / os.sysconf("SC_CLK_TCK")
    except Exception as e:
        return False, f"could not read /proc: {e}"
    STATE["server_rss_mb"], STATE["server_cpu_s"] = rss, cpu_s
    return rss < 300, f"RSS {rss:.0f} MB, CPU time so far {cpu_s:.1f} s"


async def t_op_end_under_load():
    clients = STATE["load"]
    op = STATE["load_op"]
    for c in clients:
        c.take("op_ended"); c.take("channel_deleted")
    s, b = api(STATE["officer"].token, "DELETE", f"/ops/{op['id']}")
    ended = await asyncio.gather(*[c.wait_for("op_ended", 5) for c in clients])
    n = sum(1 for e in ended if e)
    # Voice to a deleted net must go nowhere
    ch = op["nets"][0]["channel_id"]
    for c in clients:
        c.rx.clear()
    clients[0].send_voice(ch)
    await drain(0.3)
    leaked = sum(len(c.rx) for c in clients)
    return s == 200 and n == len(clients) and leaked == 0, f"end={s}, op_ended to {n}/{len(clients)}, voice into deleted net delivered={leaked}"


async def t_churn_reconnect():
    """20 users drop and reconnect with their JWT while others keep talking."""
    clients = STATE["load"][:20]
    general = STATE["general"]
    talker = STATE["small"]["alice"]
    stop = False

    async def chatter():
        while not stop:
            talker.send_voice(general)
            await asyncio.sleep(0.02)
    task = asyncio.create_task(chatter())
    ok = 0
    for c in clients:
        await c.close()
    await drain(0.3)
    for c in clients:
        c2 = new_client(c.name)
        if await c2.login_token(c.token):
            j, _ = await c2.join(general)
            if j and await c2.wait_keys():
                c2.send_keepalive(general)
                ok += 1
        STATE.setdefault("rejoined", []).append(c2)
    await drain(0.5)
    for c in STATE["rejoined"]:
        c.rx.clear()
    await drain(0.6)
    stop = True
    await task
    hearing = sum(1 for c in STATE["rejoined"] if any(r.ok for r in c.rx))
    return ok == len(clients) and hearing == len(clients), f"reconnected {ok}/{len(clients)}, hearing audio {hearing}/{len(clients)}"


async def t_health_after():
    r = requests.get(f"{BASE}/health", timeout=5)
    alive = STATE["server_proc"].poll() is None
    return r.status_code == 200 and alive, f"/health={r.status_code}, process alive={alive}"


# ─────────────────────────── setup ───────────────────────────

def build_helpers():
    out = HERE / ".build"
    out.mkdir(exist_ok=True)
    interop = out / "interop"
    r = subprocess.run(["g++", "-std=c++17", "-O1", "-I", str(CLIENT_SRC),
                        str(HERE / "interop.cpp"), str(CLIENT_SRC / "crypto/srtp_session.cpp"),
                        str(CLIENT_SRC / "crypto/key_exchange.cpp"),
                        "-I/usr/include/x86_64-linux-gnu/qt6", "-I/usr/include/x86_64-linux-gnu/qt6/QtCore",
                        "-fPIC", "-lssl", "-lcrypto", "-lQt6Core", "-o", str(interop)], capture_output=True, text=True)
    STATE["interop"] = str(interop) if r.returncode == 0 else None
    if r.returncode:
        print(r.stderr[-2000:])
    q = "/usr/include/x86_64-linux-gnu/qt6"
    moc = shutil.which("moc") or "/usr/lib/qt6/libexec/moc"
    subprocess.run([moc, str(CLIENT_SRC / "ui/overlay/talk_overlay.h"), "-o", str(out / "moc_ov.cpp")], check=False)
    ov = out / "overlay_check"
    r = subprocess.run(["g++", "-std=c++17", "-fPIC", "-I", str(CLIENT_SRC), f"-I{q}", f"-I{q}/QtCore", f"-I{q}/QtGui", f"-I{q}/QtWidgets",
                        str(HERE / "overlay_check.cpp"), str(CLIENT_SRC / "ui/overlay/talk_overlay.cpp"), str(out / "moc_ov.cpp"),
                        "-lQt6Widgets", "-lQt6Gui", "-lQt6Core", "-o", str(ov)], capture_output=True, text=True)
    STATE["overlay"] = str(ov) if r.returncode == 0 else None
    if r.returncode:
        print(r.stderr[-2000:])


def run_overlay_checks():
    exe = STATE.get("overlay")
    if not exe:
        record("overlay harness build", "overlay", False, "could not build")
        return
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    out = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=30).stdout
    for line in out.splitlines():
        if line.startswith("{"):
            d = json.loads(line)
            record(d["name"], "overlay", d["ok"], d["detail"])


def start_server():
    subprocess.run(["su", "postgres", "-c", "psql -q -c 'DROP DATABASE IF EXISTS voipsim WITH (FORCE);' -c 'CREATE DATABASE voipsim OWNER voip;'"], check=True)
    env = dict(os.environ,
               VOIP__DATABASE__URL="postgresql://voip:voip@localhost:5432/voipsim",
               VOIP__DATABASE__MAX_CONNECTIONS="30",
               VOIP__SERVER__BIND_ADDRESS=HOST, VOIP__SERVER__CONTROL_PORT=str(PORT), VOIP__SERVER__VOICE_PORT=str(VPORT),
               VOIP__SECURITY__JWT_SECRET=secrets.token_hex(32),
               VOIP__BOOTSTRAP__ORG_NAME="SCDA", VOIP__BOOTSTRAP__ORG_TAG=ORG,
               VOIP__BOOTSTRAP__ADMIN_USERNAME="admin", VOIP__BOOTSTRAP__ADMIN_PASSWORD=ADMIN_PW,
               VOIP__DISCORD__CLIENT_ID="sim-client", VOIP__DISCORD__CLIENT_SECRET="sim-secret",
               VOIP__DISCORD__REDIRECT_URI=f"{BASE}/auth/discord/callback", VOIP__DISCORD__GUILD_ID=GUILD,
               VOIP__DISCORD__API_BASE=f"http://{HOST}:{MOCK_PORT}/api",
               VOIP__DISCORD__AUTHORIZE_URL=f"http://{HOST}:{MOCK_PORT}/oauth2/authorize",
               RUST_LOG="voip_server=warn")
    exe = SERVER_DIR / "target" / "release" / "voip-server"
    if not exe.exists():
        exe = SERVER_DIR / "target" / "debug" / "voip-server"
    log = open(HERE / ".build" / "server.log", "w")
    proc = subprocess.Popen([str(exe)], cwd=SERVER_DIR, env=env, stdout=log, stderr=subprocess.STDOUT)
    STATE["server_proc"], STATE["server_exe"] = proc, exe.parent.name
    for _ in range(100):
        try:
            if requests.get(f"{BASE}/health", timeout=1).status_code == 200:
                return
        except Exception:
            pass
        time.sleep(0.1)
    raise RuntimeError("server did not start; see .build/server.log")


async def seed():
    admin = new_client("admin")
    assert await admin.login_password("admin", ADMIN_PW), admin.auth
    STATE["admin"] = admin
    t = admin.token
    roles = {}
    for name, perms, drole in [("Command", 3, DROLE_CMD), ("Pilot", 3, DROLE_PILOT), ("Marine", 3, None), ("Officer", 3 | 256, None)]:
        body = {"name": name, "permissions": perms, "priority": 1}
        if drole:
            body["discord_role_id"] = drole
        s, b = api(t, "POST", "/admin/roles", body)
        assert s in (200, 201), (s, b)
        roles[name] = b["role"]["id"]
    STATE["role_pilot"], STATE["roles"] = roles["Pilot"], roles

    s, b = api(t, "POST", "/admin/roles", {"name": "Bad", "permissions": 1, "discord_role_id": "abc"})
    STATE["bad_discord_role_status"] = s

    # Bulk-create users (one admin call), then assign roles directly in SQL: fixture setup only,
    # so the per-user admin API rate limit (60/min) isn't spent on seeding.
    n = STATE["n"]
    plan = [(u, None) for u in ("alice", "bob", "carol", "dave", "erin", "frank")] + [("officer1", "Officer")]
    plan += [(f"sim{i:03d}", "Command" if i < n * 0.1 else "Pilot" if i < n * 0.6 else "Marine") for i in range(n)]
    for k in range(0, len(plan), 50):
        chunk = [{"username": u, "password": "Passw0rd!x", "display_name": u, "is_admin": False} for u, _ in plan[k:k + 50]]
        s, b = api(t, "POST", "/admin/users/bulk", chunk)
        assert s in (200, 201) and not b.get("failed"), (s, b)
    values = ",".join(f"('{u}','{r}')" for u, r in plan if r)
    sql = (f"INSERT INTO user_roles (user_id, role_id) SELECT u.id, r.id FROM (VALUES {values}) v(un, rn) "
           f"JOIN users u ON u.username = v.un JOIN roles r ON r.name = v.rn AND r.org_id = u.org_id;")
    subprocess.run(["su", "postgres", "-c", f'psql -q -d voipsim -c "{sql}"'], check=True)

    s, b = api(t, "GET", "/admin/channels")
    STATE["general"] = next(c["id"] for c in b["channels"] if c["name"] == "General")
    s, b = api(t, "POST", "/admin/channels", {"name": "Private"})
    STATE["private"] = b["channel"]["id"]

    officer = await logged_in("officer1")
    STATE["officer"] = officer
    R = roles
    tmpl = {"name": "Fleet Op", "nets": [
        {"name": "Command", "priority": 1, "roles": [{"role_id": R["Command"], "access": "talk"}, {"role_id": R["Officer"], "access": "talk"},
                                                     {"role_id": R["Pilot"], "access": "listen"}, {"role_id": R["Marine"], "access": "listen"}]},
        {"name": "Wing A", "priority": 3, "pan": -0.6, "radio": True, "roles": [{"role_id": R["Pilot"], "access": "talk"}, {"role_id": R["Command"], "access": "listen"}]},
        {"name": "Wing B", "priority": 3, "pan": 0.6, "radio": True, "roles": [{"role_id": R["Pilot"], "access": "talk"}]},
        {"name": "Ground", "priority": 4, "roles": [{"role_id": R["Marine"], "access": "talk"}, {"role_id": R["Command"], "access": "listen"}]},
    ]}
    s, b = api(officer.token, "POST", "/ops/templates", tmpl)
    assert s == 201, (s, b)
    STATE["template_id"] = b["template"]["id"]
    # A channel only Discord-synced Command role may join (for role sync test)
    s, b = api(t, "POST", "/ops/templates", {"name": "Cmd only", "nets": [{"name": "Cmd Net", "roles": [{"role_id": R["Command"], "access": "talk"}]}]})
    s, b = api(officer.token, "POST", "/ops", {"template_id": b["template"]["id"], "name": "CmdOnly"})
    STATE["cmd_only"] = b["op"]["nets"][0]["channel_id"]


async def t_bad_discord_role_rejected():
    s = STATE.get("bad_discord_role_status")
    return s == 400, f"non-numeric discord_role_id -> {s}"


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--users", type=int, default=100)
    ap.add_argument("--speakers", type=int, default=12)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--target", type=float, default=0.95)
    ap.add_argument("--sample", type=int, default=1, help="capacity mode: fully decrypt 1 in N packets at load listeners")
    a = ap.parse_args()
    STATE.update(n=a.users, speakers=a.speakers, seconds=a.seconds, sample=a.sample)

    print("Building helpers (C++ interop + overlay checks)...")
    build_helpers()
    mock = ThreadingHTTPServer((HOST, MOCK_PORT), MockHandler)
    threading.Thread(target=mock.serve_forever, daemon=True).start()
    print("Starting server...")
    start_server()
    print(f"Seeding {a.users} users...")
    await seed()

    print("\nRunning scenarios\n")
    await run_test("crypto", "C++ client -> simulator (key derivation + decrypt)", t_interop_cpp_to_py)
    await run_test("crypto", "Simulator -> C++ client decrypt; speaker id bound", t_interop_py_to_cpp)

    STATE["small"] = await setup_small()
    await run_test("voice", "A->B delivery on shared channel", t_basic_delivery)
    await run_test("voice", "Silent listener receives (keepalive registration)", t_listener_without_talking)
    await run_test("voice", "Speaker does not hear own echo", t_no_echo)
    await run_test("voice", "Transmit into un-joined channel is dropped", t_non_member_dropped)
    await run_test("voice", "Forged sender id dropped; victim not hijacked", t_forged_sender_dropped)
    await run_test("voice", "Two speakers, identical seq: both decrypt", t_simultaneous_speakers_same_seq)
    await run_test("voice", "Multi-channel audio tagged with right channel", t_multi_channel_tagging)
    await run_test("voice", "Leaving a channel stops its audio", t_leave_stops_audio)
    await run_test("voice", "Rapid multi-join (Join op) keeps keys in sync", t_rapid_multi_join_keys)

    await run_test("discord", "/auth/config advertises Discord", t_discord_config)
    await run_test("discord", "First login creates account, WS auth works", t_discord_new_user)
    await run_test("discord", "Display name taken from guild nickname", t_discord_display_name)
    await run_test("discord", "Second login links the same account", t_discord_relogin_links)
    await run_test("discord", "Non-guild member refused", t_discord_non_member)
    await run_test("discord", "Cancelled consent reported to client", t_discord_cancelled)
    await run_test("discord", "Wrong verifier can't collect; ticket collected once", t_discord_wrong_verifier)
    await run_test("discord", "Ticket is single-use", t_discord_ticket_single_use)
    await run_test("discord", "Ticket refused for another org", t_discord_org_mismatch)
    await run_test("discord", "Username collision gets a suffix", t_discord_username_collision)
    await run_test("discord", "Discord API failure handled", t_discord_api_failure)
    await run_test("discord", "Bad challenge / unknown org rejected", t_discord_bad_challenge)
    await run_test("discord", "Disabled account refused", t_discord_disabled_account)
    await run_test("discord", "Role sync grants then revokes access", t_discord_role_sync)
    await run_test("discord", "Non-numeric Discord role id rejected", t_bad_discord_role_rejected)

    run_overlay_checks()

    await run_test("load", f"{a.users} concurrent logins", t_load_logins, timeout=300)
    await run_test("load", "Op start notifies every member", t_op_start_notifies_all)
    await run_test("load", "Everyone auto-joins assigned nets with right access", t_op_join_assigned, timeout=180)
    await run_test("load", f"Voice delivery >= 95% ({a.speakers} speakers, {a.seconds:g}s @ 50pps)", t_load_voice, timeout=180)
    await run_test("load", "Latency p95 < 50 ms", t_load_latency)
    await run_test("load", "No decrypt failures at listeners", t_load_integrity)
    await run_test("load", "Listen-only enforced under load", t_listen_only_enforced_under_load)
    await run_test("load", "Server memory within budget", t_server_resources)
    await run_test("load", "Op end under load cleans up", t_op_end_under_load)
    await run_test("churn", "20 users drop + reconnect mid-voice", t_churn_reconnect, timeout=180)
    await run_test("churn", "Server healthy after everything", t_health_after)

    passed = sum(r["ok"] for r in RESULTS)
    total = len(RESULTS)
    rate = passed / total if total else 0
    print(f"\n{passed}/{total} passed ({rate:.1%}); target {a.target:.0%} -> {'MET' if rate >= a.target else 'NOT MET'}")
    meta = dict(passed=passed, total=total, rate=rate, target=a.target, users=a.users, speakers=a.speakers, sample=a.sample,
                seconds=a.seconds, server_build=STATE.get("server_exe"), load=STATE.get("load_stats"),
                server_rss_mb=STATE.get("server_rss_mb"), server_cpu_s=STATE.get("server_cpu_s"))
    (HERE / ".build" / "results.json").write_text(json.dumps(dict(meta=meta, results=RESULTS), indent=2))
    STATE["server_proc"].terminate()
    mock.shutdown()
    return 0 if rate >= a.target else 1


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    finally:
        p = STATE.get("server_proc")
        if p and p.poll() is None:
            p.terminate()
