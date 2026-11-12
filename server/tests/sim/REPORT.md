# DadLink simulation report

Run on 13 Nov 2026 against the release server build with Postgres 16, in a 2-core Linux container. Simulated clients speak the real protocol: WebSocket control, X25519 key exchange and AES-128-GCM encrypted UDP voice. Discord is a local mock of its OAuth and API endpoints. Overlay checks run the real overlay code offscreen.

## Result: 45/45 passed (100%), target 95%: **met**

Standard run: `python3 run_sim.py --users 100 --speakers 12 --seconds 5`

| Group | Check | Result | Detail |
|---|---|---|---|
| crypto | C++ client -> simulator (key derivation + decrypt) | PASS | keys match=True, plaintext=b'dadlink-interop' |
| crypto | Simulator -> C++ client decrypt; speaker id bound | PASS | decrypt='from-python', wrong speaker id -> FAIL |
| voice | A->B delivery on shared channel | PASS | bob received 50/50 from alice, decrypt failures=0 |
| voice | Silent listener receives (keepalive registration) | PASS | silent listener received 20/20 |
| voice | Speaker does not hear own echo | PASS | echoed packets: 0 |
| voice | Transmit into un-joined channel is dropped | PASS | packets leaked into channel carol isn't in: 0 |
| voice | Forged sender id dropped; victim not hijacked | PASS | forged delivered=0, alice still receives 10/10 |
| voice | Two speakers, identical seq: both decrypt | PASS | listener decrypted 10/10 + 10/10 with identical seqs, failures=0 |
| voice | Multi-channel audio tagged with right channel | PASS | general 10/10, private 10/10 (correctly tagged) |
| voice | Leaving a channel stops its audio | PASS | after leaving received 0; rejoined=True |
| voice | Rapid multi-join (Join op) keeps keys in sync | PASS | received 10/10, decrypt failures=0, key exchanges=1, errors=[] |
| discord | /auth/config advertises Discord | PASS | {'discord': True} |
| discord | First login creates account, WS auth works | PASS | ws auth=True, user_id=109, callback=200 |
| discord | Display name taken from guild nickname | PASS | display_name='Viper-1' (guild nick) |
| discord | Second login links the same account | PASS | second login user_id=109, first=109 |
| discord | Non-guild member refused | PASS | {'reason': "You need to be a member of the org's Discord server to log in.", 'status': 'error'}, callback=403 |
| discord | Cancelled consent reported to client | PASS | {'reason': 'Discord login was cancelled (access_denied).', 'status': 'error'} |
| discord | Wrong verifier can't collect; ticket collected once | PASS | wrong verifier=pending, right=ok, second collect=pending |
| discord | Ticket is single-use | PASS | first use=True, reuse=False (Discord login expired or already used. Please try again.) |
| discord | Ticket refused for another org | PASS | ticket for scda used on other-org accepted=False (This Discord login was for a different organisation) |
| discord | Username collision gets a suffix | PASS | created usernames include alice2=True |
| discord | Discord API failure handled | PASS | {'reason': 'Discord rejected the login. Please try again.', 'status': 'error'}, callback=403 |
| discord | Bad challenge / unknown org rejected | PASS | bad challenge=400, unknown org=404 |
| discord | Disabled account refused | PASS | admin disable=200, then {'reason': 'Your DadLink account is disabled. Ask an admin.', 'status': 'error'} |
| discord | Role sync grants then revokes access | PASS | with Discord role: joined=True; after removal: joined=False (forbidden) |
| discord | Non-numeric Discord role id rejected | PASS | non-numeric discord_role_id -> 400 |
| overlay | overlay_idle_invisible | PASS | 1x1 |
| overlay | overlay_idle_tag | PASS | 68x38 |
| overlay | overlay_row_height | PASS | 82 |
| overlay | overlay_caps_at_10_plus_more | PASS | 258 |
| overlay | overlay_scale | PASS | 76 |
| overlay | overlay_corner_anchor | PASS | screen 800x800 overlay 609,700 161x60 |
| overlay | overlay_net_colours | PASS |  |
| overlay | overlay_disable_hides | PASS |  |
| load | 100 concurrent logins | PASS | 100/100 logged in, 1372 ms total |
| load | Op start notifies every member | PASS | 100/100 notified in 76 ms |
| load | Everyone auto-joins assigned nets with right access | PASS | joins: 141 talk + 101 listen-only, mismatches/timeouts=0 |
| load | Voice delivery >= 95% (12 speakers, 5s @ 50pps) | PASS | 12 speakers -> delivered 178500/178500 (100.00%); server CPU 0.15 cores over 5.0 s send window |
| load | Latency p95 < 50 ms | PASS | p50=3.1 ms, p95=6.4 ms, p99=8.9 ms (loopback) |
| load | No decrypt failures at listeners | PASS | decrypt failures across all listeners: 0 |
| load | Listen-only enforced under load | PASS | 3 listen-only users x 50 frames on Command -> 0 delivered |
| load | Server memory within budget | PASS | RSS 75 MB, CPU time so far 5.3 s |
| load | Op end under load cleans up | PASS | end=200, op_ended to 100/100, voice into deleted net delivered=0 |
| churn | 20 users drop + reconnect mid-voice | PASS | reconnected 20/20, hearing audio 20/20 |
| churn | Server healthy after everything | PASS | /health=200, process alive=True |

## Load numbers (100 users, 12 simultaneous speakers, 5 s at 50 packets/s)

- Delivered **178,500 / 178,500** voice packets (100.00%), 0 decrypt failures
- Latency on loopback: p50 3.1 ms, p95 6.4 ms, p99 8.9 ms
- Server forwarded about 35,691 packets/s using **0.15 CPU cores**; memory 75 MB

## Capacity probe (200 users, 30 simultaneous speakers)

`python3 run_sim.py --users 200 --speakers 30 --seconds 5 --sample 20`. Listeners fully decrypt 1 in 20 packets and count the rest from the header, so the Python simulator isn't the bottleneck.

- Delivered 965,684 / 971,250 (99.43%); server used 0.82 of 2 cores
- Latency p50 289 ms, p95 810 ms. The single-threaded Python receiver handling about 190k packets/s on the same 2-core box accounts for most of this; the server wasn't CPU-bound. Re-measure on separate machines before relying on 30 simultaneous speakers. Real ops rarely have more than a handful talking at once.
- Every other check passed at 200 users (44/45).

## Bugs the simulation found (fixed)

1. **Join op silence (key exchange race).** Joining several channels back-to-back, which Join op does, started a new key exchange per join and overwrote the server's secret. The two sides derived different keys and the user heard nothing. The first run showed 0/10 packets; after the fix, 10/10.
2. **AES-GCM nonce reuse (found while writing the simulator).** Speakers sharing a sequence number were re-encrypted for a listener with the same key and nonce. The speaker id is now part of the nonce. The C++ client, Rust server and simulator are all checked to agree.

## Running it

```
cd server && cargo build --release
cd tests/sim && python3 run_sim.py            # needs local Postgres (user voip/voip), Qt6 dev, OpenSSL dev
```
