/*
 * qr_reader — Container A
 *
 * Opens a serial port (SERIAL_PORT env var, default /tmp/ttyS1) at 9600 baud
 * 8N1 and runs a TCP server on TCP_PORT (default 9000).
 *
 * TCP commands (newline-terminated):
 *   INIT  -> sends "INIT\n" to serial, reads response, replies "OK\n"
 *   PING  -> replies "PONG\n" (no serial I/O)
 *   START -> sends "START\n" to serial, waits up to 5 s for JSON QR data,
 *            forwards JSON or replies "TIMEOUT\n"
 *   STOP  -> sends "STOP\n" to serial, replies "OK\n"
 *
 * When SIMULATE=1 the serial device is still opened but START returns a
 * synthetic QR JSON payload immediately.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */
#define DEFAULT_SERIAL_PORT "/tmp/ttyS1"
#define DEFAULT_TCP_PORT    9000
#define SERIAL_BAUD         B9600
#define START_TIMEOUT_MS    5000
#define READ_BUF_SIZE       512
#define LINE_BUF_SIZE       1024

/* ------------------------------------------------------------------ */
/* Logging helpers                                                      */
/* ------------------------------------------------------------------ */
static void log_ts(const char *level, const char *msg)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", t);
    fprintf(stderr, "[%s] [%s] %s\n", ts, level, msg);
    fflush(stderr);
}

#define LOG_INFO(msg)  log_ts("INFO ",  msg)
#define LOG_WARN(msg)  log_ts("WARN ",  msg)
#define LOG_ERROR(msg) log_ts("ERROR",  msg)

/* ------------------------------------------------------------------ */
/* Serial port                                                          */
/* ------------------------------------------------------------------ */
static int serial_fd = -1;
static const char *serial_port_path = DEFAULT_SERIAL_PORT;

static int serial_open(void)
{
    int fd = open(serial_port_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "open(%s): %s", serial_port_path, strerror(errno));
        LOG_WARN(msg);
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "tcgetattr: %s", strerror(errno));
        LOG_WARN(msg);
        close(fd);
        return -1;
    }

    cfsetispeed(&tty, SERIAL_BAUD);
    cfsetospeed(&tty, SERIAL_BAUD);

    /* 8N1, no flow control */
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |=  (CS8 | CREAD | CLOCAL);

    /* Raw input */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tty.c_oflag &= ~OPOST;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "tcsetattr: %s", strerror(errno));
        LOG_WARN(msg);
        close(fd);
        return -1;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Serial port %s opened (fd=%d)", serial_port_path, fd);
    LOG_INFO(msg);
    return fd;
}

static void serial_close(void)
{
    if (serial_fd >= 0) {
        close(serial_fd);
        serial_fd = -1;
        LOG_INFO("Serial port closed");
    }
}

/* Ensure serial is open; retry until success */
static void serial_ensure_open(void)
{
    while (serial_fd < 0) {
        serial_fd = serial_open();
        if (serial_fd < 0) {
            LOG_WARN("Retrying serial open in 2s…");
            sleep(2);
        }
    }
}

/* Write a line to serial; reopen on error */
static int serial_write_line(const char *line)
{
    serial_ensure_open();
    ssize_t len = (ssize_t)strlen(line);
    ssize_t n = write(serial_fd, line, (size_t)len);
    if (n != len) {
        char msg[256];
        snprintf(msg, sizeof(msg), "serial write error: %s — reopening", strerror(errno));
        LOG_WARN(msg);
        serial_close();
        serial_ensure_open();
        n = write(serial_fd, line, (size_t)len);
        if (n != len) {
            LOG_ERROR("serial write failed after reopen");
            return -1;
        }
    }
    return 0;
}

/*
 * Read a line from serial within timeout_ms milliseconds.
 * Returns number of bytes placed in buf (0-terminated), or -1 on error/timeout.
 */
static int serial_read_line(char *buf, size_t bufsz, int timeout_ms)
{
    size_t pos = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (pos < bufsz - 1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long rem_ms = (deadline.tv_sec  - now.tv_sec)  * 1000L
                    + (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (rem_ms <= 0)
            return -1; /* timeout */

        struct pollfd pfd = { .fd = serial_fd, .events = POLLIN };
        int r = poll(&pfd, 1, (int)(rem_ms > 100 ? 100 : rem_ms));
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG_WARN("poll on serial failed");
            serial_close();
            serial_ensure_open();
            return -1;
        }
        if (r == 0) continue; /* re-check deadline */

        char c;
        ssize_t n = read(serial_fd, &c, 1);
        if (n <= 0) {
            if (n == 0 || errno == EAGAIN) continue;
            LOG_WARN("serial read error — reopening");
            serial_close();
            serial_ensure_open();
            return -1;
        }
        if (c == '\n') {
            buf[pos] = '\0';
            return (int)pos;
        }
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* ------------------------------------------------------------------ */
/* QR simulation helper                                                 */
/* ------------------------------------------------------------------ */
static void make_simulated_qr(char *buf, size_t bufsz)
{
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char code[7];
    for (int i = 0; i < 6; i++)
        code[i] = charset[rand() % (int)(sizeof(charset) - 1)];
    code[6] = '\0';
    long long ts = (long long)time(NULL);
    snprintf(buf, bufsz,
             "{\"qr-data\":{\"code\":\"%s\",\"ts\":%lld}}",
             code, ts);
}

/* ------------------------------------------------------------------ */
/* TCP helpers                                                          */
/* ------------------------------------------------------------------ */

/* Send a string to a TCP client fd */
static int tcp_send(int fd, const char *s)
{
    size_t len = strlen(s);
    ssize_t n = write(fd, s, len);
    if (n < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "tcp write error: %s", strerror(errno));
        LOG_WARN(msg);
        return -1;
    }
    return 0;
}

/*
 * Read one newline-terminated line from TCP client into buf.
 * Returns length (>= 0) or -1 on disconnect/error.
 */
static int tcp_read_line(int fd, char *buf, size_t bufsz)
{
    size_t pos = 0;
    while (pos < bufsz - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\r') continue; /* tolerate CRLF */
        if (c == '\n') break;
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* ------------------------------------------------------------------ */
/* Command handler                                                      */
/* ------------------------------------------------------------------ */
static int simulate = 0;

static void handle_command(int client_fd, const char *cmd)
{
    char msg[1088];
    snprintf(msg, sizeof(msg), "CMD from TCP client: '%s'", cmd);
    LOG_INFO(msg);

    if (strcmp(cmd, "PING") == 0) {
        tcp_send(client_fd, "PONG\n");

    } else if (strcmp(cmd, "INIT") == 0) {
        serial_write_line("INIT\n");
        /* Read (and discard) any serial response within 2 s */
        char sbuf[READ_BUF_SIZE];
        serial_read_line(sbuf, sizeof(sbuf), 2000);
        tcp_send(client_fd, "OK\n");

    } else if (strcmp(cmd, "START") == 0) {
        if (simulate) {
            char qr[256];
            make_simulated_qr(qr, sizeof(qr));
            char reply[280];
            snprintf(reply, sizeof(reply), "%s\n", qr);
            tcp_send(client_fd, reply);
        } else {
            serial_write_line("START\n");
            char sbuf[READ_BUF_SIZE];
            int r = serial_read_line(sbuf, sizeof(sbuf), START_TIMEOUT_MS);
            if (r < 0 || sbuf[0] == '\0') {
                tcp_send(client_fd, "TIMEOUT\n");
            } else {
                char reply[READ_BUF_SIZE + 4];
                snprintf(reply, sizeof(reply), "%s\n", sbuf);
                tcp_send(client_fd, reply);
            }
        }

    } else if (strcmp(cmd, "STOP") == 0) {
        serial_write_line("STOP\n");
        char sbuf[READ_BUF_SIZE];
        serial_read_line(sbuf, sizeof(sbuf), 2000);
        tcp_send(client_fd, "OK\n");

    } else {
        char reply[1040];
        snprintf(reply, sizeof(reply), "UNKNOWN_CMD:%s\n", cmd);
        tcp_send(client_fd, reply);
    }
}

/* ------------------------------------------------------------------ */
/* Signal handling                                                      */
/* ------------------------------------------------------------------ */
static volatile int running = 1;

static void handle_sigterm(int sig)
{
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */
int main(void)
{
    srand((unsigned)time(NULL));

    signal(SIGTERM, handle_sigterm);
    signal(SIGINT,  handle_sigterm);
    signal(SIGPIPE, SIG_IGN);

    /* Read configuration from environment */
    const char *env_port = getenv("SERIAL_PORT");
    if (env_port && env_port[0]) serial_port_path = env_port;

    int tcp_port = DEFAULT_TCP_PORT;
    const char *env_tcp = getenv("TCP_PORT");
    if (env_tcp && env_tcp[0]) tcp_port = atoi(env_tcp);

    const char *env_sim = getenv("SIMULATE");
    if (env_sim && strcmp(env_sim, "1") == 0) {
        simulate = 1;
        LOG_INFO("SIMULATE=1: synthetic QR responses enabled");
    }

    char infomsg[256];
    snprintf(infomsg, sizeof(infomsg),
             "Starting qr_reader  serial=%s  tcp_port=%d  simulate=%d",
             serial_port_path, tcp_port, simulate);
    LOG_INFO(infomsg);

    /* Open serial port (non-fatal if not available yet) */
    serial_fd = serial_open();
    if (serial_fd < 0)
        LOG_WARN("Serial not available at startup — will retry on first use");

    /* Create TCP server socket */
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)tcp_port);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv_fd);
        return 1;
    }

    if (listen(srv_fd, 8) < 0) {
        perror("listen");
        close(srv_fd);
        return 1;
    }

    snprintf(infomsg, sizeof(infomsg), "TCP server listening on port %d", tcp_port);
    LOG_INFO(infomsg);

    /* Accept loop */
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* Use poll so we can check 'running' periodically */
        struct pollfd pfd = { .fd = srv_fd, .events = POLLIN };
        int r = poll(&pfd, 1, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (r == 0) continue;

        int client_fd = accept(srv_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        snprintf(infomsg, sizeof(infomsg), "Client connected: %s:%d",
                 client_ip, ntohs(client_addr.sin_port));
        LOG_INFO(infomsg);

        /* Serve this client until disconnect */
        char line[LINE_BUF_SIZE];
        while (running) {
            int n = tcp_read_line(client_fd, line, sizeof(line));
            if (n < 0) {
                LOG_INFO("Client disconnected");
                break;
            }
            if (n == 0) continue; /* empty line */
            handle_command(client_fd, line);
        }

        close(client_fd);
    }

    LOG_INFO("Shutting down");
    serial_close();
    close(srv_fd);
    return 0;
}
