FROM alpine:3.19 AS builder

RUN apk add --no-cache gcc musl-dev

WORKDIR /build
COPY src/main.c .
RUN gcc -O2 -Wall -Wextra -pthread -o qr-c main.c

# ---- runtime ----
FROM alpine:3.19

# socat runs as a sidecar inside this container so the PTY device nodes
# live in the same /dev/pts namespace as the C reader. Cross-container
# PTY sharing via a bind-mounted symlink does not work in default Docker.
RUN apk add --no-cache libgcc socat

WORKDIR /app
COPY --from=builder /build/qr-c .
COPY start.sh .
RUN chmod +x start.sh

ENV SERIAL_PORT=/tmp/ttyS1
ENV BAUD_RATE=115200
ENV SOCK_PATH=/tmp/qr.sock
ENV READ_TIMEOUT=10

CMD ["./start.sh"]
