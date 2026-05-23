# bloq.it Gateway — Embedded Challenge

Smart parcel locker gateway: simulates QR-scanner hardware (C/C++) and bridges it to the cloud via MQTT over TLS (Python).

## Architecture

```
                  mosquitto  (local, port 1883)
        │  from_cloud/command  ▼
        │                   Container B — gateway-py (Python)
        │  from_device/events  ▲     │  Unix socket /tmp/qr.sock
        │                            ▼
                             Container A — qr-c
                              ├─ socat sidecar  /tmp/ttyS1 ↔ /tmp/ttyS2
                              └─ C reader      reads /tmp/ttyS1
```

The compose stack ships its own `eclipse-mosquitto` broker so the gateway has a deterministic, rate-limit-free MQTT endpoint. Plain MQTT on `1883` is used inside the docker network; the broker port is published to `localhost:11883` (remapped from the container's `1883` to avoid colliding with a system-installed mosquitto on the host) so you can `mosquitto_pub`/`mosquitto_sub` from the host. The gateway can still be pointed at `test.mosquitto.org:8883` by setting `MQTT_HOST=test.mosquitto.org MQTT_PORT=8883 MQTT_USE_TLS=true` — the cert is downloaded at build time.

socat runs as a sidecar **inside the qr-c container** (started by `start.sh`) so the PTY device nodes live in the same `/dev/pts` namespace as the C reader. Cross-container PTY sharing via a bind-mounted symlink does not work under default Docker isolation.

See [`diagrams/arquitetura.mmd`](diagrams/arquitetura.mmd) for the full Mermaid diagram.

## Prerequisites

- Docker ≥ 24 and Docker Compose v2

## Run

```bash
docker compose up --build --remove-orphans
```

`--remove-orphans` clears any leftover container from an older compose definition (e.g. a previous `socat` service name).

Three services start in order: `mosquitto` → `qr-c` (with socat sidecar) → `gateway-py`, gated by health checks.

## Troubleshooting

### `gateway-py` keeps logging `MQTT disconnected: reason_code=Unspecified error`

This is paho's generic "connection lost" code. The gateway runs a TLS preflight on startup that logs the real cause:
```
TLS preflight OK — protocol=TLSv1.3 cipher=TLS_AES_256_GCM_SHA384
```
If you see `TLS preflight FAILED — <ExceptionType>: <message>` instead, the message tells you whether it's DNS, cert verification, or a network reachability problem. The most common causes:

- **Stale client ID**: another connection on the broker is using the same `MQTT_CLIENT_ID`. This repo uses a random UUID suffix by default, so it shouldn't happen unless you set `MQTT_CLIENT_ID` explicitly via env.
- **Outbound TCP/8883 blocked** (only relevant when `MQTT_USE_TLS=true` against `test.mosquitto.org`): corporate firewall or VPN. Try `docker compose exec gateway-py python -c "import socket; socket.create_connection(('test.mosquitto.org', 8883), 5)"`.
- **Stale CA cert**: re-build with `docker compose build --no-cache gateway-py` to refresh `/certs/mosquitto.org.crt`.

## Test

### 1 — Send a command from the cloud side

The compose stack runs a local `mosquitto` broker on `localhost:1883`. Publish to `from_cloud/command` with any MQTT client (e.g. mosquitto_pub):

```bash
# Install mosquitto clients if needed: brew install mosquitto / apt install mosquitto-clients

# Watch responses in one terminal
mosquitto_sub -h localhost -p 11883 -t from_device/events -v

# In another terminal, send a PING
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"PING"}'
```

You should see the gateway's `gateway_online` event followed by the PING response on the events topic.

### 2 — Inject a mock QR code (simulate the scanner hardware)

`/tmp/ttyS2` is the other end of the socat pair.  Writing to it simulates the physical QR scanner emitting a barcode string.

Chain the START and the inject so the scan arrives before `READ_TIMEOUT`
expires (default 30 s — see env table below). Run from the repo root so
`docker compose ps` can resolve the container; otherwise the command
substitution is empty and docker exec errors with `No such container: sh`.

```bash
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"START"}' \
  && docker exec $(docker compose ps -q qr-c) sh -c 'echo ABC123 > /tmp/ttyS2'

# Alternative if you're not in the repo root — look up the container name:
# docker exec $(docker ps --filter name=qr-c --format '{{.Names}}') \
#   sh -c 'echo ABC123 > /tmp/ttyS2'
```

Expected event on `from_device/events`:
```json
{"qr-data": {"code": "ABC123", "ts": 1730780000}}
```

### 3 — Full parcel delivery cycle

End-to-end simulation of a courier drop-off followed by a customer pick-up.
Open **two terminals** before starting.

**Terminal A — watch all events (keep open throughout):**
```bash
mosquitto_sub -h localhost -p 11883 -t from_device/events -v
```

---

#### Phase 0 — Health check

Verify the gateway is live before touching the locker:

```bash
# Terminal B
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"PING"}'
```

Expected on Terminal A:
```json
{"response": "PONG", "command": "PING", "ts": 1730780000}
```

---

#### Phase 1 — Courier drop-off

The cloud has pre-generated a one-time QR token for the courier (`COURIER-001`).
It now arms the scanner and waits for the courier to present their phone.

```bash
# Terminal B — arm the scanner, then immediately inject the courier's QR
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"START"}' \
  && docker exec $(docker compose ps -q qr-c) sh -c 'echo COURIER-001 > /tmp/ttyS2'
```

Expected on Terminal A:
```json
{"qr-data": {"code": "COURIER-001", "ts": 1730780000}}
```

The cloud platform receives the code, validates it against the pre-issued token,
and would now unlock the door (MCU/relay — outside this simulation). After the
courier loads the parcel and closes the door the cloud sends STOP to confirm the
session is closed:

```bash
# Terminal B — cloud closes the courier session
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"STOP"}'
```

Expected on Terminal A:
```json
{"response": "STOP", "command": "STOP", "ts": 1730780000}
```

---

#### Phase 2 — Customer pick-up

The cloud arms the scanner again for the customer's separate QR token (`CUSTOMER-001`):

```bash
# Terminal B — arm the scanner, then immediately inject the customer's QR
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"START"}' \
  && docker exec $(docker compose ps -q qr-c) sh -c 'echo CUSTOMER-001 > /tmp/ttyS2'
```

Expected on Terminal A:
```json
{"qr-data": {"code": "CUSTOMER-001", "ts": 1730780000}}
```

The cloud validates the token, unlocks the door, and the customer retrieves their parcel.

```bash
# Terminal B — cloud closes the customer session
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"STOP"}'
```

---

#### Phase 3 — Abort a scan mid-wait (optional)

Send START then cancel before any QR arrives — simulates the cloud aborting an
open session (e.g. token expired):

```bash
# Terminal B — arm, then cancel 3 seconds later
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"START"}' \
  && sleep 3 \
  && mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"STOP"}'
```

Expected on Terminal A — STOP acknowledgement (no qr-data published):
```json
{"response": "STOP", "command": "STOP", "ts": 1730780000}
```

---

#### Phase 4 — Re-initialise the serial reader (optional)

If the serial reader gets into a bad state, INIT resets it without restarting the container:

```bash
# Terminal B
mosquitto_pub -h localhost -p 11883 -t from_cloud/command -m '{"command":"INIT"}'
```

Expected on Terminal A:
```json
{"response": "OK", "command": "INIT", "ts": 1730780000}
```

---

### 4 — Test the Unix socket directly (no MQTT needed)

```bash
docker exec -it <qr-c-container> sh
echo "PING" | nc -U /tmp/qr.sock
# → PONG
echo "INIT" | nc -U /tmp/qr.sock
# → OK
```

### 5 — View logs

```bash
docker compose logs -f
```

## Environment variables

### qr-c (Container A)

| Variable       | Default        | Description                             |
|----------------|----------------|-----------------------------------------|
| `SERIAL_PORT`  | `/dev/ttyS1`   | Path to the serial device               |
| `BAUD_RATE`    | `115200`       | UART baud rate                          |
| `SOCK_PATH`    | `/tmp/qr.sock` | Unix socket path exposed to Container B |
| `READ_TIMEOUT` | `30`           | Seconds to wait for a QR scan (START)   |

### gateway-py (Container B)

| Variable            | Default                    | Description                                          |
|---------------------|----------------------------|------------------------------------------------------|
| `MQTT_HOST`         | `mosquitto`                | MQTT broker hostname (service name on compose net)   |
| `MQTT_PORT`         | `1883`                     | MQTT broker port (1883 plain, 8883 TLS)              |
| `MQTT_USE_TLS`      | `false`                    | Set to `true` for the `test.mosquitto.org` TLS path  |
| `MQTT_CERT`         | `/certs/mosquitto.org.crt` | CA certificate used when `MQTT_USE_TLS=true`         |
| `MQTT_TOPIC_CMD`    | `from_cloud/command`       | Topic subscribed for commands                        |
| `MQTT_TOPIC_EVENT`  | `from_device/events`       | Topic published for events                           |
| `SOCK_PATH`         | `/tmp/qr.sock`             | Unix socket path to Container A                      |
| `RECONNECT_DELAY`   | `5`                        | Seconds between MQTT reconnect attempts              |

## Design decisions

**IPC — Unix socket (`/tmp/qr.sock`)**  
Shared via a Docker volume (`/tmp:/tmp`) across both containers.  A Unix socket has zero network overhead, no port conflicts, and is trivial to health-check (`test -S`).  The alternative (TCP/REST) would require an HTTP library in the C code; a second socat serial pair would add compose complexity with no benefit.

**Container A threading model**  
Each IPC client connection is handled in a detached `pthread`.  A `volatile stop_flag` lets a concurrent `STOP` command interrupt an in-progress `START` read loop.  The serial file descriptor is protected by a mutex so multiple rapid `PING`s do not race.

**Serial reopening**  
`ensure_serial()` is called inside every `handle_start()` loop iteration under the mutex, so transient disconnects (USB unplug/replug) recover automatically without restarting the container.

**Local broker (default) instead of `test.mosquitto.org`**  
The compose stack ships an `eclipse-mosquitto:2` broker. The public broker is rate-limited and was observed to close inbound CONNECTs from this environment before sending CONNACK, producing opaque `Unspecified error` disconnects in paho. A local broker eliminates that whole class of issues and makes the test loop deterministic. Set `MQTT_USE_TLS=true` together with `MQTT_HOST=test.mosquitto.org MQTT_PORT=8883` to switch back — the gateway loads `/certs/mosquitto.org.crt` (mosquitto's private CA, downloaded at build time) on top of system CAs.

**`paho-mqtt` v2 API**  
`CallbackAPIVersion.VERSION2` is used to avoid the deprecation warning and for future-proofness.

## Future improvements

- Add a TLS listener to the local `mosquitto` broker (self-signed CA, mounted into both broker and gateway) so the dev setup matches the spec's encrypted-transport requirement without depending on the public broker
- Add an authenticated user/password (or per-device client cert) to the local broker so the gateway is exercised against a non-anonymous config
- Add a proper message queue between IPC and MQTT so commands during a reconnect are not dropped
- Support MQTT v5 properties (message expiry, correlation data)
- Replace the simulated serial with a real `/dev/ttyUSBx` device by overriding `SERIAL_PORT`
- Add Prometheus metrics endpoint to Container B for observability
- Sign MQTT payloads (HMAC or JWT) so the cloud can verify device identity
