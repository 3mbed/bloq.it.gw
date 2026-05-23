FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# mosquitto.org public CA cert — only consulted when MQTT_USE_TLS=true and
# the gateway is pointed at test.mosquitto.org. We keep it baked in so the
# TLS path is one env flip away; if the download fails (no internet at
# build time), drop a placeholder and rely on system CAs at runtime.
RUN apt-get update -qq \
    && apt-get install -y --no-install-recommends wget ca-certificates \
    && mkdir -p /certs \
    && (wget -q https://test.mosquitto.org/ssl/mosquitto.org.crt -O /certs/mosquitto.org.crt \
        || echo "# placeholder — using system CAs at runtime" > /certs/mosquitto.org.crt) \
    && apt-get purge -y wget \
    && rm -rf /var/lib/apt/lists/*

COPY app.py .

# Defaults target the local broker shipped by docker-compose.yml.
# Override these to point at test.mosquitto.org:8883 with MQTT_USE_TLS=true.
ENV MQTT_HOST=mosquitto
ENV MQTT_PORT=1883
ENV MQTT_USE_TLS=false
ENV MQTT_CERT=/certs/mosquitto.org.crt
ENV MQTT_TOPIC_CMD=from_cloud/command
ENV MQTT_TOPIC_EVENT=from_device/events
ENV SOCK_PATH=/tmp/qr.sock
ENV RECONNECT_DELAY=5

CMD ["python", "-u", "app.py"]
