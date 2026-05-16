"""
gateway-py — Container B

Bridges MQTT commands from the cloud to the qr-c TCP server (Container A)
and forwards responses back as MQTT events.

Environment variables:
  QR_HOST          hostname of Container A (default: qr-c)
  QR_PORT          TCP port of Container A (default: 9000)
  MQTT_BROKER      MQTT broker hostname   (default: test.mosquitto.org)
  MQTT_PORT        MQTT broker TLS port   (default: 8883)
  MQTT_CA_CERT     path to CA cert file   (default: /certs/mosquitto.org.crt)
  MQTT_CLIENT_ID   MQTT client id         (default: bloqit-gw-<random>)
"""

from __future__ import annotations

import json
import logging
import os
import socket
import ssl
import threading
import time
import uuid
from typing import Optional

import paho.mqtt.client as mqtt

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
log = logging.getLogger("gateway-py")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
QR_HOST       = os.environ.get("QR_HOST", "qr-c")
QR_PORT       = int(os.environ.get("QR_PORT", "9000"))
MQTT_BROKER   = os.environ.get("MQTT_BROKER", "test.mosquitto.org")
MQTT_PORT     = int(os.environ.get("MQTT_PORT", "8883"))
MQTT_CA_CERT  = os.environ.get("MQTT_CA_CERT", "/certs/mosquitto.org.crt")
_default_id   = f"bloqit-gw-{str(uuid.uuid4())[:8]}"
MQTT_CLIENT_ID = os.environ.get("MQTT_CLIENT_ID", _default_id)

TOPIC_CMD    = "from_cloud/command"
TOPIC_EVENTS = "from_device/events"

PING_INTERVAL = 30  # seconds between health-check PINGs


# ---------------------------------------------------------------------------
# QRClient — TCP connection to Container A
# ---------------------------------------------------------------------------
class QRClient:
    """Thread-safe TCP client for the qr-c server."""

    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self._sock: Optional[socket.socket] = None
        self._lock = threading.Lock()

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------
    def _connect(self) -> None:
        """Open a fresh TCP connection (called with lock held)."""
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect((self.host, self.port))
        s.settimeout(None)  # blocking mode for recv
        self._sock = s
        log.info("Connected to qr-c at %s:%d", self.host, self.port)

    def _ensure_connected(self) -> None:
        """Connect with exponential back-off if not already connected."""
        delay = 1
        while self._sock is None:
            try:
                self._connect()
            except OSError as exc:
                log.warning("Cannot connect to qr-c: %s — retrying in %ds", exc, delay)
                time.sleep(delay)
                delay = min(delay * 2, 30)

    # ------------------------------------------------------------------
    # Send / receive
    # ------------------------------------------------------------------
    def _send_line(self, cmd: str) -> None:
        """Send a newline-terminated command (lock must be held)."""
        assert self._sock is not None
        self._sock.sendall((cmd.strip() + "\n").encode())

    def _recv_line(self) -> str:
        """Read until newline (lock must be held). Returns stripped line."""
        assert self._sock is not None
        buf = b""
        self._sock.settimeout(8)
        try:
            while b"\n" not in buf:
                chunk = self._sock.recv(1024)
                if not chunk:
                    raise ConnectionError("qr-c closed connection")
                buf += chunk
        finally:
            self._sock.settimeout(None)
        return buf.split(b"\n", 1)[0].decode(errors="replace").strip()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------
    def send_command(self, cmd: str) -> str:
        """Send *cmd* to qr-c and return the response line."""
        with self._lock:
            retries = 2
            last_exc: Optional[Exception] = None
            for attempt in range(retries):
                try:
                    self._ensure_connected()
                    self._send_line(cmd)
                    response = self._recv_line()
                    log.info("qr-c  cmd=%s  resp=%s", cmd, response)
                    return response
                except (OSError, ConnectionError) as exc:
                    last_exc = exc
                    log.warning(
                        "qr-c communication error (attempt %d): %s — reconnecting",
                        attempt + 1,
                        exc,
                    )
                    # Force reconnect on next iteration
                    if self._sock is not None:
                        try:
                            self._sock.close()
                        except OSError:
                            pass
                        self._sock = None
            raise ConnectionError(f"qr-c command failed after {retries} attempts: {last_exc}")


# ---------------------------------------------------------------------------
# MQTT helpers
# ---------------------------------------------------------------------------
def make_event(source: str, **kwargs) -> str:
    payload = {"source": source, "ts": int(time.time()), **kwargs}
    return json.dumps(payload)


def publish_event(client: mqtt.Client, payload: str) -> None:
    result = client.publish(TOPIC_EVENTS, payload, qos=1)
    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        log.warning("MQTT publish failed: rc=%d", result.rc)


# ---------------------------------------------------------------------------
# Background health-check thread
# ---------------------------------------------------------------------------
def health_check_loop(qr: QRClient, client: mqtt.Client) -> None:
    """Send PING to qr-c every PING_INTERVAL seconds."""
    while True:
        time.sleep(PING_INTERVAL)
        try:
            resp = qr.send_command("PING")
            payload = make_event("qr", health=resp)
        except Exception as exc:  # noqa: BLE001
            log.error("Health-check PING failed: %s", exc)
            payload = make_event("gateway", error=f"health-check failed: {exc}")
        publish_event(client, payload)


# ---------------------------------------------------------------------------
# MQTT callbacks
# ---------------------------------------------------------------------------
def on_connect(client: mqtt.Client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        log.info("MQTT connected to %s:%d", MQTT_BROKER, MQTT_PORT)
        client.subscribe(TOPIC_CMD, qos=1)
        log.info("Subscribed to %s", TOPIC_CMD)
    else:
        log.warning("MQTT connection refused: reason_code=%s", reason_code)


def on_disconnect(client: mqtt.Client, userdata, disconnect_flags, reason_code, properties):
    log.warning("MQTT disconnected: reason_code=%s — paho will reconnect", reason_code)


def on_message(client: mqtt.Client, userdata: QRClient, msg: mqtt.MQTTMessage) -> None:
    qr: QRClient = userdata
    raw = msg.payload.decode(errors="replace").strip()
    log.info("MQTT message on %s: %s", msg.topic, raw)

    # Parse command: accept {"command": "START"} or plain "START"
    cmd = raw
    try:
        parsed = json.loads(raw)
        if isinstance(parsed, dict) and "command" in parsed:
            cmd = str(parsed["command"]).upper()
    except (json.JSONDecodeError, ValueError):
        cmd = raw.upper()

    try:
        response = qr.send_command(cmd)
        payload = make_event("qr", response=response, command=cmd)
    except Exception as exc:  # noqa: BLE001
        log.error("Error forwarding command %s to qr-c: %s", cmd, exc)
        payload = make_event("gateway", error=str(exc), command=cmd)

    publish_event(client, payload)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main() -> None:
    log.info(
        "gateway-py starting  qr=%s:%d  broker=%s:%d  client_id=%s",
        QR_HOST, QR_PORT, MQTT_BROKER, MQTT_PORT, MQTT_CLIENT_ID,
    )

    qr = QRClient(QR_HOST, QR_PORT)

    # ---- MQTT client setup ----
    client = mqtt.Client(
        client_id=MQTT_CLIENT_ID,
        protocol=mqtt.MQTTv311,
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        userdata=qr,
    )

    # TLS
    tls_ctx = ssl.create_default_context(cafile=MQTT_CA_CERT)
    client.tls_set_context(tls_ctx)

    # Callbacks
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    # Reconnect settings (paho v2 uses reconnect_delay_set)
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    # Connect (non-blocking loop_start handles reconnects automatically)
    log.info("Connecting to MQTT broker %s:%d …", MQTT_BROKER, MQTT_PORT)
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_start()

    # Background health-check thread
    t = threading.Thread(target=health_check_loop, args=(qr, client), daemon=True)
    t.start()

    log.info("Gateway running — waiting for MQTT commands on '%s'", TOPIC_CMD)

    # Block main thread (paho network loop is in its own thread)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        log.info("Interrupted — shutting down")
    finally:
        client.loop_stop()
        client.disconnect()
        log.info("Gateway stopped")


if __name__ == "__main__":
    main()
