"""End-to-end test of op templates, live ops and channel ACL enforcement."""
import asyncio, json, sys
import requests, websockets

BASE = "http://127.0.0.1:9000"
WS = "ws://127.0.0.1:9000/ws"
ok = 0
def check(cond, label):
    global ok
    if not cond:
        print("FAIL:", label); sys.exit(1)
    ok += 1; print("  ok -", label)

class Client:
    def __init__(self, name): self.name = name; self.inbox = []
    async def login(self, username, password):
        self.ws = await websockets.connect(WS)
        await self.ws.send(json.dumps({"type": "authenticate", "payload": {
            "method": "password", "username": username, "password": password, "org_tag": "scda"}}))
        while True:
            m = json.loads(await asyncio.wait_for(self.ws.recv(), 5))
            if m["type"] == "auth_success": self.auth = m["payload"]; break
            if m["type"] in ("auth_failed", "error"): raise RuntimeError(m)
        self.token = self.auth["token"]
        await asyncio.sleep(0.3); await self.drain()
        return self
    async def drain(self, wait=0.3):
        try:
            while True:
                self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), wait)))
        except asyncio.TimeoutError: pass
    def take(self, t):
        out = [m for m in self.inbox if m["type"] == t]; self.inbox = [m for m in self.inbox if m["type"] != t]; return out
    async def join(self, ch):
        await self.ws.send(json.dumps({"type": "join_channel", "payload": {"channel_id": ch}}))
        await self.drain(0.5)
        j = self.take("channel_joined"); e = self.take("error")
        return (j[0]["payload"] if j else None), (e[0]["payload"] if e else None)
    def h(self): return {"Authorization": f"Bearer {self.token}"}

def api(c, method, path, body=None):
    r = requests.request(method, BASE + path, headers=c.h(), json=body)
    try: return r.status_code, r.json()
    except Exception: return r.status_code, {}

async def main():
    admin = await Client("admin").login("admin", "TestPass123")
    check(admin.auth["is_admin"] is True and admin.auth["permissions"] == 0x1FF, "auth_success carries is_admin + permissions")

    # Roles + users
    roles = {}
    for name, perms in [("Command", 3), ("Pilot", 3), ("Officer", 3 | 256)]:
        s, b = api(admin, "POST", "/admin/roles", {"name": name, "permissions": perms, "priority": 1, "color": "#ffffff"})
        check(s in (200, 201), f"create role {name}"); roles[name] = b["role"]["id"]
    users = {}
    for uname, role in [("pilot1", "Pilot"), ("cmd1", "Command"), ("officer1", "Officer"), ("nobody1", None)]:
        s, b = api(admin, "POST", "/admin/users", {"username": uname, "password": "Passw0rd!x", "display_name": uname, "is_admin": False})
        check(s in (200, 201), f"create user {uname}"); users[uname] = b["user"]["id"]
        if role:
            s, _ = api(admin, "POST", f"/admin/roles/{roles[role]}/assign/{users[uname]}")
            check(s in (200, 201), f"assign {role} to {uname}")

    officer = await Client("officer").login("officer1", "Passw0rd!x")
    check(officer.auth["permissions"] & 256 and not officer.auth["is_admin"], "officer has OpLeader bit, not admin")
    s, _ = api(officer, "GET", "/admin/users"); check(s == 403, "officer blocked from admin API")
    s, b = api(officer, "GET", "/ops/roles"); check(s == 200 and len(b["roles"]) == 3, "officer can list roles via /ops/roles")

    pilot = await Client("pilot").login("pilot1", "Passw0rd!x")
    s, _ = api(pilot, "GET", "/ops/templates"); check(s == 403, "pilot blocked from ops API")

    R = roles
    template = {"name": "Standard Op", "description": "Command + Fleet", "nets": [
        {"name": "Command", "priority": 1, "pan": 0, "radio": False,
         "roles": [{"role_id": R["Command"], "access": "talk"}, {"role_id": R["Officer"], "access": "talk"},
                   {"role_id": R["Pilot"], "access": "listen"}]},
        {"name": "Fleet", "priority": 3, "pan": -0.6, "radio": True,
         "roles": [{"role_id": R["Pilot"], "access": "talk"}, {"role_id": R["Command"], "access": "talk"},
                   {"role_id": R["Officer"], "access": "talk"}]},
        {"name": "Open Net", "roles": []},
    ]}
    s, b = api(officer, "POST", "/ops/templates", {**template, "nets": template["nets"] + [{"name": "fleet"}]})
    check(s == 400 and "Duplicate" in b["error"], "duplicate net name rejected")
    s, b = api(officer, "POST", "/ops/templates", {**template, "nets": [{"name": "X", "roles": [{"role_id": 9999, "access": "talk"}]}]})
    check(s == 400 and "Unknown role" in b["error"], "unknown role rejected")
    s, b = api(officer, "POST", "/ops/templates", {**template, "nets": [{"name": "X", "pan": 3}]})
    check(s == 400, "out-of-range pan rejected")
    s, b = api(officer, "POST", "/ops/templates", template)
    check(s == 201 and len(b["template"]["nets"]) == 3, "officer creates template")
    tid = b["template"]["id"]
    fleet = [n for n in b["template"]["nets"] if n["name"] == "Fleet"][0]
    check(fleet["radio"] is True and abs(fleet["pan"] + 0.6) < 1e-6 and fleet["priority"] == 3, "net audio defaults stored")
    s, _ = api(officer, "POST", "/ops/templates", template); check(s == 409, "duplicate template name -> 409")

    # Edit: rename + keep nets
    s, b = api(officer, "PUT", f"/ops/templates/{tid}", {**template, "name": "Standard Op v2"})
    check(s == 200 and b["template"]["name"] == "Standard Op v2", "template update")

    # Start op: pilot online should be told
    s, b = api(officer, "POST", "/ops", {"template_id": tid, "name": "Nightfall"})
    check(s == 201, f"start op ({s} {b.get('error')})")
    op = b["op"]; parent = op["parent_channel_id"]
    nets = {n["name"]: n["channel_id"] for n in op["nets"]}
    check(set(nets) == {"Command", "Fleet", "Open Net"}, "op has 3 nets")
    await pilot.drain(0.6)
    upd = pilot.take("channel_updated"); started = pilot.take("op_started")
    check(len(upd) == 4, "pilot got channel_updated for lobby + 3 nets")
    check(len(started) == 1, "pilot got op_started")
    pv = {n["name"]: n for n in started[0]["payload"]["op"]["nets"]}
    check(pv["Command"]["access"] == "listen" and pv["Command"]["assigned"], "pilot: Command = listen, assigned")
    check(pv["Fleet"]["access"] == "talk" and pv["Fleet"]["radio"] and pv["Fleet"]["pan"] < 0, "pilot: Fleet = talk with radio/pan defaults")
    check(pv["Open Net"]["access"] == "talk" and not pv["Open Net"]["assigned"], "pilot: open net talkable, not auto-joined")

    s, _ = api(officer, "POST", "/ops", {"template_id": tid, "name": "Nightfall"}); check(s == 409, "duplicate op name -> 409")

    # ACL enforcement on join
    j, e = await pilot.join(nets["Command"]); check(j and j["can_speak"] is False, "pilot joins Command listen-only")
    j, e = await pilot.join(nets["Fleet"]); check(j and j["can_speak"] is True, "pilot joins Fleet with talk")
    nobody = await Client("nobody").login("nobody1", "Passw0rd!x")
    j, e = await nobody.join(nets["Command"]); check(j is None and e and e["code"] == "forbidden", "no-role user refused from Command")
    j, e = await nobody.join(nets["Open Net"]); check(j and j["can_speak"], "no-role user can join open net")
    j, e = await nobody.join(parent); check(j and j["can_speak"], "no-role user can join op lobby")
    j, e = await admin.join(nets["Command"]); check(j and j["can_speak"], "admin bypasses ACL")
    # Pre-existing channel without ACL still open
    general = [c for c in nobody.auth["channels"] if c["name"] == "General"][0]["id"]
    j, e = await nobody.join(general); check(j and j["can_speak"], "existing channel without ACL unchanged")

    # Late joiner gets the op on login
    cmd = await Client("cmd").login("cmd1", "Passw0rd!x")
    late = [m for m in cmd.inbox if m["type"] == "op_started"]
    check(len(late) == 1 and {n["name"]: n["access"] for n in late[0]["payload"]["op"]["nets"]}["Command"] == "talk",
          "late login receives op_started (Command = talk)")
    s, b = api(officer, "GET", "/ops"); check(s == 200 and len(b["ops"]) == 1, "list active ops")

    # End op
    s, b = api(officer, "DELETE", f"/ops/{op['id']}"); check(s == 200 and len(b["deleted_channels"]) == 4, "end op deletes 4 channels")
    await pilot.drain(0.6)
    dels = {m["payload"]["channel_id"] for m in pilot.take("channel_deleted")}
    check(dels == set(nets.values()) | {parent}, "pilot told all op channels deleted")
    check(len(pilot.take("op_ended")) == 1, "pilot got op_ended")
    s, b = api(officer, "GET", "/ops"); check(len(b["ops"]) == 0, "no active ops after end")
    s, _ = api(officer, "DELETE", f"/ops/{op['id']}"); check(s == 404, "ending twice -> 404")
    s, b = api(admin, "GET", "/admin/channels")
    check(not any(c["id"] in dels for c in b["channels"]), "op channels gone from DB")
    # Op name reusable after it ended
    s, b = api(officer, "POST", "/ops", {"template_id": tid, "name": "Nightfall"}); check(s == 201, "op name reusable after end")
    api(officer, "DELETE", f"/ops/{b['op']['id']}")
    s, _ = api(officer, "DELETE", f"/ops/templates/{tid}"); check(s == 200, "delete template")
    print(f"\nALL {ok} CHECKS PASSED")

asyncio.run(main())
