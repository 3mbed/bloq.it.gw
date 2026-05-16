FROM python:3.11-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
# Download mosquitto.org TLS CA certificate
RUN mkdir -p /certs && \
    python3 -c "import urllib.request; urllib.request.urlretrieve('https://test.mosquitto.org/ssl/mosquitto.org.crt', '/certs/mosquitto.org.crt')"
COPY app.py .
ENV QR_HOST=qr-c
ENV QR_PORT=9000
ENV MQTT_BROKER=test.mosquitto.org
ENV MQTT_PORT=8883
ENV MQTT_CA_CERT=/certs/mosquitto.org.crt
CMD ["python3", "-u", "app.py"]
