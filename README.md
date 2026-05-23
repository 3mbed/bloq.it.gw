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


### Full parcel delivery cycle Sequence test

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

### View logs

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


## Design decisions

**IPC — Unix socket (`/tmp/qr.sock`)**  
Shared via a Docker volume (`/tmp:/tmp`) across both containers.  A Unix socket has zero network overhead, no port conflicts, and is trivial to health-check (`test -S`).  The alternative (TCP/REST) would require an HTTP library in the C code; a second socat serial pair would add compose complexity with no benefit.

**Container A threading model**  
Each IPC client connection is handled in a detached `pthread`.  A `volatile stop_flag` lets a concurrent `STOP` command interrupt an in-progress `START` read loop.  The serial file descriptor is protected by a mutex so multiple rapid `PING`s do not race.

**Serial reopening**  
`ensure_serial()` is called inside every `handle_start()` loop iteration under the mutex, so transient disconnects (USB unplug/replug) recover automatically without restarting the container.

**Local broker (default) instead of `test.mosquitto.org`**  
The compose stack ships an `eclipse-mosquitto:2` broker. The public broker is rate-limited and was observed to close inbound CONNECTs from this environment before sending CONNACK, producing opaque `Unspecified error` disconnects in paho. A local broker eliminates that whole class of issues and makes the test loop deterministic. Set `MQTT_USE_TLS=true` together with `MQTT_HOST=test.mosquitto.org MQTT_PORT=8883` to switch back — the gateway loads `/certs/mosquitto.org.crt` (mosquitto's private CA, downloaded at build time) on top of system CAs.

**MQTT Broker TLS issue**
 
The compose stack ships its own `eclipse-mosquitto` broker so the gateway has a deterministic, rate-limit-free MQTT endpoint. Plain MQTT on `1883` is used inside the docker network; the broker port is published to `localhost:11883` (remapped from the container's `1883` to avoid colliding with a system-installed mosquitto on the host) so you can `mosquitto_pub`/`mosquitto_sub` from the host. The gateway can still be pointed at `test.mosquitto.org:8883` by setting `MQTT_HOST=test.mosquitto.org MQTT_PORT=8883 MQTT_USE_TLS=true` — the cert is downloaded at build time. 

Later I can Add a TLS listener to the local `mosquitto` broker (self-signed CA, mounted into both broker and gateway) so the dev setup matches the spec's encrypted-transport requirement without depending on the public broker

**`paho-mqtt` v2 API**  
`CallbackAPIVersion.VERSION2` is used to avoid the deprecation warning and for future-proofness.