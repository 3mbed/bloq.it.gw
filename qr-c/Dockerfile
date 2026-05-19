FROM alpine:3.19 AS builder

RUN apk add --no-cache gcc musl-dev

WORKDIR /build
COPY src/main.c .
RUN gcc -O2 -Wall -Wextra -pthread -o qr-c main.c

# ---- runtime ----
FROM alpine:3.19

RUN apk add --no-cache libgcc

WORKDIR /app
COPY --from=builder /build/qr-c .

ENV SERIAL_PORT=/tmp/ttyS1
ENV BAUD_RATE=115200
ENV SOCK_PATH=/tmp/qr.sock
ENV READ_TIMEOUT=10

CMD ["./qr-c"]
