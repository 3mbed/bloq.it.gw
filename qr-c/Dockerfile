FROM alpine:3.19
RUN apk add --no-cache gcc musl-dev
WORKDIR /app
COPY src/main.c src/health_check.c ./
RUN gcc -O2 -Wall -o qr_reader main.c && \
    gcc -O2 -Wall -o health_check health_check.c
ENV SERIAL_PORT=/tmp/ttyS1
ENV TCP_PORT=9000
ENV SIMULATE=0
HEALTHCHECK --interval=10s --timeout=5s --start-period=5s --retries=3 \
    CMD ["/app/health_check"]
CMD ["./qr_reader"]
