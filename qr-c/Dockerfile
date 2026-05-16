FROM alpine:3.19
RUN apk add --no-cache gcc musl-dev make
WORKDIR /app
COPY src/main.c .
RUN gcc -O2 -Wall -o qr_reader main.c
ENV SERIAL_PORT=/tmp/ttyS1
ENV TCP_PORT=9000
ENV SIMULATE=0
CMD ["./qr_reader"]
