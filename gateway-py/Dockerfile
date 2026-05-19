FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Download mosquitto.org public CA cert (TLS peer verification)
RUN apt-get update -qq && apt-get install -y --no-install-recommends wget ca-certificates \
    && wget -q https://test.mosquitto.org/ssl/mosquitto.org.crt -O /certs/mosquitto.org.crt 2>/dev/null \
    || (mkdir -p /certs && echo "# placeholder — mount real cert or rely on system CAs" > /certs/mosquitto.org.crt) \
    && apt-get purge -y wget && rm -rf /var/lib/apt/lists/*

COPY app.py .

ENV MQTT_HOST=test.mosquitto.org
ENV MQTT_PORT=8883
ENV MQTT_CERT=/certs/mosquitto.org.crt
ENV MQTT_TOPIC_CMD=from_cloud/command
ENV MQTT_TOPIC_EVENT=from_device/events
ENV SOCK_PATH=/tmp/qr.sock
ENV RECONNECT_DELAY=5

CMD ["python", "-u", "app.py"]
