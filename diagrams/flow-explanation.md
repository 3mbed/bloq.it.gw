# System Flow Explanation — The Life of a Parcel

End-to-end walkthrough of the 12-step smart locker flow, mapped to the
actual code in each container.

---

## Before the story starts — `docker compose up`

Three services start in strict order, gated by health checks:

```
docker compose up
│
├─1─▶ fake-serial (alpine/socat)
│       socat pty,link=/tmp/ttyS1 ↔ pty,link=/tmp/ttyS2
│       health: test -e /tmp/ttyS1
│
├─2─▶ qr-c  (Container A — C)          [waits for fake-serial healthy]
│       open_serial("/tmp/ttyS1")
│       listen("/tmp/qr.sock")
│       health: test -S /tmp/qr.sock
│
└─3─▶ gateway-py  (Container B — Python) [waits for qr-c healthy]
        client.connect("test.mosquitto.org", 8883, TLS)
        on_connect → subscribe("from_cloud/command")
        on_connect → publish("from_device/events", {"event":"gateway_online"})
```

---

## Steps 1 & 2 — Cloud issues QR tokens

```
  [ Cloud platform ]
   token gen & validation
        │
   ┌────┴────┐
   ▼         ▼
[Courier]  [Customer]
 phone app   phone app
 "ABC123"    "XYZ789"
```

Entirely cloud-side. Our code is idle — Container B just holds the MQTT
connection alive with `keepalive=60`. No code in our repo runs here.

---

## Steps 3 & 4 — Someone presents their QR to the scanner

```
[Courier]          [Customer]
   │                    │
   │  holds phone up    │  holds phone up
   ▼                    ▼
        [ QR Scanner ]
         hardware serial module
         detects barcode optically
              │
              │  emits "ABC123\n"
              │  over UART TX pin
              ▼
         /tmp/ttyS1           ← socat bridges ttyS1 ↔ ttyS2
         (Container A reads)    (mock: echo ABC123 > /tmp/ttyS2)
```

But Container A must be in **START** mode first. The cloud sent
`{"command":"START"}` on `from_cloud/command` → Container B routed it →
Container A is now blocked inside `serial_readline()` with a 10-second
countdown.

---

## Step 5 — UART decode lands in Container A (`qr-c/src/main.c`)

```
/tmp/ttyS1  ──────────────────▶  handle_start()
                                      │
                                  serial_readline(fd, ...)
                                  select() unblocks
                                  reads "ABC123\n" byte by byte
                                      │
                                  snprintf(resp, ...)
                                      │
                                      ▼
                        {"qr-data":{"code":"ABC123","ts":1730780000}}
```

If the 10 s timeout fires before any data, it replies:
`{"qr-data":{"code":"TIMEOUT","ts":...}}`

If the serial port drops (USB unplug), `ensure_serial()` reopens it on the
next loop iteration — no container restart needed.

---

## Step 6 — IPC: Container A → Container B via Unix socket

```
  Container A (C)                    Container B (Python)
  ────────────────                   ────────────────────
  handle_start()                     send_to_qr("START")
       │                                     │
       │  write(client, resp)                │  s.recv(4096)
       │──────── /tmp/qr.sock ─────────────▶│
       │         Unix socket                 │
       │         (shared /tmp volume)        │  returns JSON string
       │                                     ▼
       │                              on_message() parses it
       │                              calls _publish_event()
```

No TCP stack. No network hop. Just a kernel pipe through the shared `/tmp`
volume mounted by both containers.

---

## Step 7 — MQTT TLS event published to cloud (`gateway-py/app.py`)

```
  Container B                    test.mosquitto.org:8883
  ──────────                     ──────────────────────
  _publish_event()
       │
       │  client.publish(
       │    "from_device/events",
       │    '{"qr-data":{"code":"ABC123","ts":...}}',
       │    qos=1
       │  )
       │
       └────── TLS encrypted ──────────────────────────▶ [ MQTT Broker ]
                port 8883                                       │
                CA cert: mosquitto.org.crt                      │
                                                         cloud subscriber
                                                         receives event
```

On disconnect, `on_disconnect` fires → outer `while True` in `main()` waits
`RECONNECT_DELAY` seconds and reconnects. Any error is also published to
`from_device/events` so the cloud always knows device state.

---

## Steps 8–12 — Cloud validates, door opens (hardware layer)

```
[ Cloud platform ]
  validates "ABC123" ✓
       │
       │  sends unlock cmd
       ▼
  ⑧  [ MCU ]  ◀──── SBC → MCU serial/GPIO
       │
       │  GPIO pin HIGH
       ▼
  ⑨  [ Relay ]   (power switch)
       │
       │  energises solenoid
       ▼
  ⑩  [ Electric lock ]   latch retracts → door free

  ⑪  [ Door sensor ]   reed switch → "door open" → MCU

  ⑫  [ Display + Buzzer ]   "Place your parcel" + beep
                              ▲
                              └── SBC drives via I2C/UART
```

Steps 9–12 are real-time firmware on the MCU — outside our containers.
Step ⑧'s return command arrives as another message on `from_cloud/command`,
which Container B receives in `on_message()` and routes to Container A just
like any other command.

---

## Summary — which file owns which step

```
Step  │ File                    │ Function
──────┼─────────────────────────┼──────────────────────────────────
  5   │ qr-c/src/main.c         │ open_serial(), serial_readline()
  5   │ qr-c/src/main.c         │ handle_start() — select() read loop
  6   │ qr-c/src/main.c         │ write(client, resp)  ← IPC out
  6   │ gateway-py/app.py       │ send_to_qr()         ← IPC in
  6   │ gateway-py/app.py       │ on_message() — command routing
  7   │ gateway-py/app.py       │ _publish_event()     ← MQTT publish
  7   │ gateway-py/app.py       │ on_connect/on_disconnect — reconnect
 3-4  │ docker-compose.yml      │ fake-serial socat — simulates scanner
```
