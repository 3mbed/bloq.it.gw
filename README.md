# bloq.it Smart Locker Gateway

Containerised simulation of the bloq.it smart-locker edge gateway.
A C-based QR-reader service (Container A) is bridged to an MQTT cloud broker
by a Python gateway (Container B), with a `socat`-generated PTY pair
emulating the hardware serial link.

---

## Architecture

```
diagrams/arquitetura.mmd
```

```mermaid
flowchart TD
    subgraph host["Host / Docker Network"]
        subgraph vol["serial_pair volume  (/tmp)"]
            S1["/tmp/ttyS1"]
            S2["/tmp/ttyS2"]
        end

        subgraph socat_svc["Container: socat"]
            SOC["socat\n(PTY pair creator)"]
        end

        subgraph qrc_svc["Container: qr-c  (C)"]
            QRC["qr_reader\n• opens SERIAL_PORT=/tmp/ttyS1\n• TCP server :9000"]
        end

        subgraph gw_svc["Container: gateway-py  (Python)"]
            GW["app.py\n• QRClient → TCP :9000\n• paho-mqtt TLS client"]
        end
    end

    subgraph cloud["Internet / Cloud"]
        MQTT["test.mosquitto.org:8883\n(TLS / MQTT broker)"]
    end

    SOC -- "creates PTY pair" --> S1
    SOC -- "creates PTY pair" --> S2
    S1 -- "serial read/write\n(SERIAL_PORT)" --> QRC
    S2 -. "other end of PTY\n(loopback / socat)" .-> SOC
    QRC -- "TCP :9000\nINIT / PING / START / STOP\n→ OK / PONG / JSON / TIMEOUT" --> GW
    GW -- "subscribe\nfrom_cloud/command" --> MQTT
    GW -- "publish\nfrom_device/events" --> MQTT
    MQTT -- "command payload\n{\"command\":\"START\"}" --> GW
    GW -- "event payload\n{\"source\":\"qr\",\"response\":\"...\",\"ts\":...}" --> MQTT
```

### Component summary

| Container | Language | Role |
|-----------|----------|------|
| `socat` | — | Creates a PTY pair at `/tmp/ttyS1` ↔ `/tmp/ttyS2` (shared volume) |
| `qr-c` | C | Reads/writes the serial port; exposes a TCP command server on `:9000` |
| `gateway-py` | Python | Connects to MQTT broker (TLS) and relays commands to `qr-c` over TCP |

---

## Prerequisites

- [Docker](https://docs.docker.com/get-docker/) ≥ 24
- [Docker Compose](https://docs.docker.com/compose/install/) v2 (`docker compose`)

---

## Quick start

```bash
docker compose up --build
```

The first build downloads Alpine/Python base images and the mosquitto TLS
certificate.  Subsequent starts are fast.

To run in the background:

```bash
docker compose up --build -d
docker compose logs -f
```

---

## Environment variables

### `qr-c` (Container A)

| Variable | Default | Description |
|----------|---------|-------------|
| `SERIAL_PORT` | `/tmp/ttyS1` | Path to the serial / PTY device |
| `TCP_PORT` | `9000` | TCP port the command server listens on |
| `SIMULATE` | `0` | Set to `1` to return synthetic QR data instead of reading the serial port |

### `gateway-py` (Container B)

| Variable | Default | Description |
|----------|---------|-------------|
| `QR_HOST` | `qr-c` | Hostname of Container A |
| `QR_PORT` | `9000` | TCP port of Container A |
| `MQTT_BROKER` | `test.mosquitto.org` | MQTT broker hostname |
| `MQTT_PORT` | `8883` | MQTT broker TLS port |
| `MQTT_CA_CERT` | `/certs/mosquitto.org.crt` | Path to the CA certificate inside the container |
| `MQTT_CLIENT_ID` | `bloqit-gw-<random>` | MQTT client identifier |

---

## MQTT topics

| Topic | Direction | Payload |
|-------|-----------|---------|
| `from_cloud/command` | cloud → gateway | `{"command":"START"}` or plain string e.g. `PING` |
| `from_device/events` | gateway → cloud | `{"source":"qr","response":"...","ts":1234567890}` |

Error events have the shape `{"source":"gateway","error":"...","ts":...}`.

---

## Manual testing

### Send a command via netcat (direct to Container A)

```bash
# PING
echo "PING" | nc -w2 localhost 9000

# Start a QR scan (returns synthetic JSON when SIMULATE=1)
echo "START" | nc -w2 localhost 9000

# Initialise the scanner
echo "INIT" | nc -w2 localhost 9000

# Stop the scanner
echo "STOP" | nc -w2 localhost 9000
```

### Publish a command via MQTT (requires `mosquitto-clients`)

```bash
mosquitto_pub \
  --cafile /path/to/mosquitto.org.crt \
  -h test.mosquitto.org -p 8883 \
  -t from_cloud/command \
  -m '{"command":"START"}'
```

### Subscribe to events

```bash
mosquitto_sub \
  --cafile /path/to/mosquitto.org.crt \
  -h test.mosquitto.org -p 8883 \
  -t from_device/events -v
```

The mosquitto CA cert can be downloaded from
<https://test.mosquitto.org/ssl/mosquitto.org.crt>.

---

## Serial pair simulation (socat)

`socat` creates two pseudo-terminal (PTY) devices and symlinks them to
`/tmp/ttyS1` and `/tmp/ttyS2` inside the shared `serial_pair` Docker volume.
Both `socat` and `qr-c` mount this volume at `/tmp`.

- `qr-c` opens `/tmp/ttyS1` as its serial port (9600 baud, 8N1).
- The other end of the PTY pair (`/tmp/ttyS2`) is owned by `socat`.
  Because nothing actively reads `/tmp/ttyS2`, a real serial command like
  `START` would produce no response and `qr-c` would return `TIMEOUT`.
- Setting `SIMULATE=1` (the default in `docker-compose.yml`) bypasses real
  serial I/O: `qr-c` generates a random QR JSON payload immediately upon
  receiving the `START` command.

To test the real serial path, set `SIMULATE=0` and attach a process (or
another socat instance) to `/tmp/ttyS2` that writes back valid JSON on
receiving `START\n`.

---

## Project structure

```
.
├── diagrams/
│   └── arquitetura.mmd      # Mermaid architecture diagram
├── gateway-py/
│   ├── app.py               # Python asyncio/paho-mqtt gateway (Container B)
│   ├── requirements.txt
│   └── Dockerfile
├── qr-c/
│   ├── src/
│   │   └── main.c           # C QR-reader / TCP server (Container A)
│   └── Dockerfile
├── docker-compose.yml
└── README.md
```
