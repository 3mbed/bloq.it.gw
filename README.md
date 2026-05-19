# bloq.it Gateway — Embedded Challenge

Smart parcel locker gateway: simulates QR-scanner hardware (C/C++) and bridges it to the cloud via MQTT over TLS (Python).

## Architecture

```
Cloud (test.mosquitto.org:8883 TLS)
        │  from_cloud/command  ▼
        │                   Container B — gateway-py (Python)
        │  from_device/events  ▲     │  Unix socket /tmp/qr.sock
        │                            ▼
                             Container A — qr-c (C/C++)
                                    │  /tmp/ttyS1 (UART)
                                    ▼
                             fake-serial (socat)
                             /tmp/ttyS1 ↔ /tmp/ttyS2
```

See [`diagrams/arquitetura.mmd`](diagrams/arquitetura.mmd) for the full Mermaid diagram.

## Prerequisites

- Docker ≥ 24 and Docker Compose v2
- Internet access to `test.mosquitto.org:8883`

## Run

```bash
docker compose up --build
```

All three services start in order: `fake-serial` → `qr-c` → `gateway-py`, gated by health checks.

## Test

### 1 — Send a command from the cloud side

Publish to `from_cloud/command` with any MQTT client (e.g. mosquitto_pub):

```bash
# Install mosquitto clients if needed: brew install mosquitto / apt install mosquitto-clients

# PING
mosquitto_pub -h test.mosquitto.org -p 8883 \
  --cafile /path/to/mosquitto.org.crt \
  -t from_cloud/command -m '{"command":"PING"}'

# Watch the response
mosquitto_sub -h test.mosquitto.org -p 8883 \
  --cafile /path/to/mosquitto.org.crt \
  -t from_device/events -v
```

### 2 — Inject a mock QR code (simulate the scanner hardware)

`/tmp/ttyS2` is the other end of the socat pair.  Writing to it simulates the physical QR scanner emitting a barcode string.

```bash
# First, trigger a START (so qr-c is waiting on the serial port)
mosquitto_pub -h test.mosquitto.org -p 8883 \
  --cafile /path/to/mosquitto.org.crt \
  -t from_cloud/command -m '{"command":"START"}'

# Then inject a fake scan (within READ_TIMEOUT seconds)
docker run --rm -v /tmp:/tmp alpine \
  sh -c 'echo ABC123 > /tmp/ttyS2'
```

Expected event on `from_device/events`:
```json
{"qr-data": {"code": "ABC123", "ts": 1730780000}}
```

### 3 — Test the Unix socket directly (no MQTT needed)

```bash
docker exec -it <qr-c-container> sh
echo "PING" | nc -U /tmp/qr.sock
# → PONG
echo "INIT" | nc -U /tmp/qr.sock
# → OK
```

### 4 — View logs

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
| `READ_TIMEOUT` | `10`           | Seconds to wait for a QR scan (START)   |

### gateway-py (Container B)

| Variable            | Default                    | Description                             |
|---------------------|----------------------------|-----------------------------------------|
| `MQTT_HOST`         | `test.mosquitto.org`       | MQTT broker hostname                    |
| `MQTT_PORT`         | `8883`                     | MQTT broker TLS port                    |
| `MQTT_CERT`         | `/certs/mosquitto.org.crt` | CA certificate for TLS verification     |
| `MQTT_TOPIC_CMD`    | `from_cloud/command`       | Topic subscribed for commands           |
| `MQTT_TOPIC_EVENT`  | `from_device/events`       | Topic published for events              |
| `SOCK_PATH`         | `/tmp/qr.sock`             | Unix socket path to Container A         |
| `RECONNECT_DELAY`   | `5`                        | Seconds between MQTT reconnect attempts |

## Design decisions

**IPC — Unix socket (`/tmp/qr.sock`)**  
Shared via a Docker volume (`/tmp:/tmp`) across both containers.  A Unix socket has zero network overhead, no port conflicts, and is trivial to health-check (`test -S`).  The alternative (TCP/REST) would require an HTTP library in the C code; a second socat serial pair would add compose complexity with no benefit.

**Container A threading model**  
Each IPC client connection is handled in a detached `pthread`.  A `volatile stop_flag` lets a concurrent `STOP` command interrupt an in-progress `START` read loop.  The serial file descriptor is protected by a mutex so multiple rapid `PING`s do not race.

**Serial reopening**  
`ensure_serial()` is called inside every `handle_start()` loop iteration under the mutex, so transient disconnects (USB unplug/replug) recover automatically without restarting the container.

**TLS without client certificates**  
`test.mosquitto.org:8883` uses server-only TLS.  We download the public CA cert at image build time; if unavailable, we fall back to the system CA bundle.  No client key/cert is needed.

**`paho-mqtt` v2 API**  
`CallbackAPIVersion.VERSION2` is used to avoid the deprecation warning and for future-proofness.

## Future improvements

- Add a proper message queue between IPC and MQTT so commands during a reconnect are not dropped
- Support MQTT v5 properties (message expiry, correlation data)
- Replace the simulated serial with a real `/dev/ttyUSBx` device by overriding `SERIAL_PORT`
- Add Prometheus metrics endpoint to Container B for observability
- Sign MQTT payloads (HMAC or JWT) so the cloud can verify device identity
