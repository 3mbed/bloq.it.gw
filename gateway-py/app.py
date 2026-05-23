"""
Container B — MQTT gateway.

Subscribes to from_cloud/command, forwards commands to Container A via
Unix socket (/tmp/qr.sock), and publishes responses to from_device/events.
"""

import json
import logging
import os
import socket
import ssl
import threading
import time
import uuid

import paho.mqtt.client as mqtt

# ---------- config ----------------------------------------------------------
MQTT_HOST        = os.getenv("MQTT_HOST",        "test.mosquitto.org")
MQTT_PORT        = int(os.getenv("MQTT_PORT",    "8883"))
MQTT_CERT        = os.getenv("MQTT_CERT",        "/certs/mosquitto.org.crt")
MQTT_TOPIC_CMD   = os.getenv("MQTT_TOPIC_CMD",   "from_cloud/command")
MQTT_TOPIC_EVENT = os.getenv("MQTT_TOPIC_EVENT", "from_device/events")
SOCK_PATH        = os.getenv("SOCK_PATH",        "/tmp/qr.sock")
# Random suffix avoids "ghost session boot" when the broker still sees
# a previous connection with the same client_id and force-disconnects us.
MQTT_CLIENT_ID   = os.getenv("MQTT_CLIENT_ID",   f"bloqit-gw-{uuid.uuid4().hex[:8]}")
RECONNECT_DELAY  = int(os.getenv("RECONNECT_DELAY", "5"))

# ---------- logging ---------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)sZ %(levelname)s %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
log = logging.getLogger("gateway")

# ---------- IPC: Unix socket to Container A ---------------------------------
_ipc_lock = threading.Lock()


def send_to_qr(command: str) -> str:
    """Send a command string to Container A and return its response line."""
    with _ipc_lock:
        for attempt in range(3):
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                    s.settimeout(30)
                    s.connect(SOCK_PATH)
                    s.sendall((command + "\n").encode())
                    data = b""
                    while not data.endswith(b"\n"):
                        chunk = s.recv(4096)
                        if not chunk:
                            break
                        data += chunk
                return data.decode().strip()
            except (ConnectionRefusedError, FileNotFoundError) as exc:
                log.warning("IPC attempt %d failed: %s", attempt + 1, exc)
                time.sleep(1)
            except Exception as exc:  # noqa: BLE001
                log.error("IPC error: %s", exc)
                return json.dumps({"error": str(exc)})
    return json.dumps({"error": "qr-c unreachable"})


# ---------- MQTT callbacks --------------------------------------------------
def _publish_event(client: mqtt.Client, payload: dict | str) -> None:
    if isinstance(payload, dict):
        payload = json.dumps(payload)
    result = client.publish(MQTT_TOPIC_EVENT, payload, qos=1)
    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        log.error("publish failed: rc=%d", result.rc)
    else:
        log.info("published → %s: %s", MQTT_TOPIC_EVENT, payload)


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        log.info("MQTT connected to %s:%d", MQTT_HOST, MQTT_PORT)
        client.subscribe(MQTT_TOPIC_CMD, qos=1)
        log.info("subscribed: %s", MQTT_TOPIC_CMD)
        _publish_event(client, {"event": "gateway_online", "ts": int(time.time())})
    else:
        log.error("MQTT connect failed: reason_code=%s", reason_code)


def on_disconnect(client, userdata, flags, reason_code, properties):
    log.warning("MQTT disconnected: reason_code=%s — will reconnect", reason_code)


def on_message(client, userdata, msg):
    raw = msg.payload.decode(errors="replace").strip()
    log.info("← %s: %s", msg.topic, raw)

    try:
        payload = json.loads(raw)
        command = payload.get("command", "").upper()
    except json.JSONDecodeError:
        command = raw.upper()

    valid_commands = {"INIT", "PING", "START", "STOP"}
    if command not in valid_commands:
        err = {"error": f"unknown command: {command}", "ts": int(time.time())}
        log.warning("rejected: %s", command)
        _publish_event(client, err)
        return

    log.info("→ qr-c: %s", command)
    response = send_to_qr(command)
    log.info("← qr-c: %s", response)

    try:
        event = json.loads(response)
    except json.JSONDecodeError:
        event = {"response": response, "command": command, "ts": int(time.time())}

    if "ts" not in event:
        event["ts"] = int(time.time())

    _publish_event(client, event)


# ---------- TLS setup -------------------------------------------------------
def _make_tls_context() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    if os.path.isfile(MQTT_CERT):
        try:
            ctx.load_verify_locations(cafile=MQTT_CERT)
            log.info("TLS: using CA cert %s", MQTT_CERT)
        except ssl.SSLError as exc:
            log.warning("TLS: cert at %s unusable (%s) — using system CAs", MQTT_CERT, exc)
    else:
        log.warning("TLS: cert file not found (%s) — using system CAs", MQTT_CERT)
    return ctx


def _preflight_tls() -> None:
    """Run a one-shot TLS handshake to surface cert/network errors clearly,
    instead of getting a generic 'Unspecified error' from paho's reconnect loop."""
    try:
        ctx = _make_tls_context()
        with socket.create_connection((MQTT_HOST, MQTT_PORT), timeout=10) as raw:
            with ctx.wrap_socket(raw, server_hostname=MQTT_HOST) as tls:
                log.info("TLS preflight OK — protocol=%s cipher=%s",
                         tls.version(), tls.cipher()[0] if tls.cipher() else "?")
    except Exception as exc:  # noqa: BLE001
        log.error("TLS preflight FAILED — %s: %s", type(exc).__name__, exc)


# ---------- main ------------------------------------------------------------
def main() -> None:
    log.info("gateway starting — MQTT=%s:%d client_id=%s",
             MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID)
    _preflight_tls()

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=MQTT_CLIENT_ID,
        clean_session=True,
    )
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    client.tls_set_context(_make_tls_context())
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    while True:
        try:
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            client.loop_forever(retry_first_connection=True)
        except Exception as exc:  # noqa: BLE001
            log.error("MQTT loop error: %s: %s — retrying in %ds",
                      type(exc).__name__, exc, RECONNECT_DELAY)
            time.sleep(RECONNECT_DELAY)


if __name__ == "__main__":
    main()
