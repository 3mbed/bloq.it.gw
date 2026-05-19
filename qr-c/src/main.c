#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>

/* ---------- configuration (env overrides) ---------- */
static const char *serial_port  = "/dev/ttyS1";
static int         baud_rate    = 115200;
static const char *sock_path    = "/tmp/qr.sock";
static int         read_timeout = 10;   /* seconds for START */

/* ---------- shared state ---------- */
static int serial_fd = -1;
static volatile int stop_flag   = 0;   /* set by STOP command */
static volatile int running     = 1;

static pthread_mutex_t serial_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---------- logging ---------- */
static void log_ts(const char *level, const char *msg)
{
    time_t now = time(NULL);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    fprintf(stdout, "[%s] %s: %s\n", buf, level, msg);
    fflush(stdout);
}

/* ---------- serial helpers ---------- */
static speed_t baud_to_speed(int baud)
{
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B115200;
    }
}

static int open_serial(void)
{
    int fd = open(serial_port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open(%s): %s", serial_port, strerror(errno));
        log_ts("ERROR", msg);
        return -1;
    }

    struct termios tty;
    tcgetattr(fd, &tty);
    speed_t spd = baud_to_speed(baud_rate);
    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);
    cfmakeraw(&tty);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tty);

    char msg[64];
    snprintf(msg, sizeof(msg), "serial opened: %s @ %d", serial_port, baud_rate);
    log_ts("INFO", msg);
    return fd;
}

/* Ensure serial_fd is open; re-open on error. Returns fd or -1. */
static int ensure_serial(void)
{
    if (serial_fd >= 0) return serial_fd;
    serial_fd = open_serial();
    return serial_fd;
}

/* ---------- command handlers ---------- */

static void handle_init(int client)
{
    pthread_mutex_lock(&serial_mutex);
    if (serial_fd >= 0) { close(serial_fd); serial_fd = -1; }
    int fd = open_serial();
    serial_fd = fd;
    pthread_mutex_unlock(&serial_mutex);

    const char *resp = (fd >= 0) ? "OK\n" : "{\"error\":\"serial open failed\"}\n";
    write(client, resp, strlen(resp));
    log_ts("CMD", fd >= 0 ? "INIT → OK" : "INIT → ERROR");
}

static void handle_ping(int client)
{
    write(client, "PONG\n", 5);
    log_ts("CMD", "PING → PONG");
}

/* Read one newline-terminated line from fd within deadline (abs time).
   Returns 0 on success, -1 on timeout/error. */
static int serial_readline(int fd, char *out, size_t maxlen, time_t deadline)
{
    size_t pos = 0;
    while (pos < maxlen - 1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        time_t now = time(NULL);
        if (now >= deadline) return -1;
        struct timeval tv = { .tv_sec = deadline - now, .tv_usec = 0 };
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) return -1;
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n' || c == '\r') {
            if (pos > 0) break;
            continue;
        }
        out[pos++] = c;
    }
    out[pos] = '\0';
    return (pos > 0) ? 0 : -1;
}

static void handle_start(int client)
{
    log_ts("CMD", "START: waiting for QR read");
    stop_flag = 0;

    time_t deadline = time(NULL) + read_timeout;
    char line[256];
    char resp[512];

    while (!stop_flag && time(NULL) < deadline) {
        pthread_mutex_lock(&serial_mutex);
        int fd = ensure_serial();
        pthread_mutex_unlock(&serial_mutex);

        if (fd < 0) {
            usleep(500000);
            continue;
        }

        pthread_mutex_lock(&serial_mutex);
        int r = serial_readline(fd, line, sizeof(line), deadline);
        pthread_mutex_unlock(&serial_mutex);

        if (r == 0 && strlen(line) > 0) {
            /* Got data from QR scanner */
            snprintf(resp, sizeof(resp),
                     "{\"qr-data\":{\"code\":\"%s\",\"ts\":%ld}}\n",
                     line, (long)time(NULL));
            write(client, resp, strlen(resp));
            log_ts("EVENT", resp);
            return;
        }

        if (stop_flag) break;
    }

    /* Timeout or STOP */
    snprintf(resp, sizeof(resp),
             "{\"qr-data\":{\"code\":\"TIMEOUT\",\"ts\":%ld}}\n",
             (long)time(NULL));
    write(client, resp, strlen(resp));
    log_ts("EVENT", stop_flag ? "STOP received during START" : "START timed out");
}

static void handle_stop(int client)
{
    stop_flag = 1;
    write(client, "OK\n", 3);
    log_ts("CMD", "STOP → OK");
}

/* ---------- client session ---------- */
static void *handle_client(void *arg)
{
    int client = *(int *)arg;
    free(arg);

    char buf[256];
    ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(client); return NULL; }
    buf[n] = '\0';

    /* strip trailing whitespace */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';

    if      (strcmp(buf, "INIT")  == 0) handle_init(client);
    else if (strcmp(buf, "PING")  == 0) handle_ping(client);
    else if (strcmp(buf, "START") == 0) handle_start(client);
    else if (strcmp(buf, "STOP")  == 0) handle_stop(client);
    else {
        char err[128];
        snprintf(err, sizeof(err), "{\"error\":\"unknown command: %s\"}\n", buf);
        write(client, err, strlen(err));
        log_ts("WARN", err);
    }

    close(client);
    return NULL;
}

/* ---------- signal handler ---------- */
static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* ---------- main ---------- */
int main(void)
{
    /* env config */
    if (getenv("SERIAL_PORT"))    serial_port  = getenv("SERIAL_PORT");
    if (getenv("BAUD_RATE"))      baud_rate    = atoi(getenv("BAUD_RATE"));
    if (getenv("SOCK_PATH"))      sock_path    = getenv("SOCK_PATH");
    if (getenv("READ_TIMEOUT"))   read_timeout = atoi(getenv("READ_TIMEOUT"));

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    log_ts("INFO", "qr-c starting");

    /* initial serial open (best effort — retry on INIT/START) */
    pthread_mutex_lock(&serial_mutex);
    serial_fd = open_serial();
    pthread_mutex_unlock(&serial_mutex);

    /* Unix socket server */
    unlink(sock_path);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    chmod(sock_path, 0666);
    listen(srv, 8);

    char msg[128];
    snprintf(msg, sizeof(msg), "listening on %s", sock_path);
    log_ts("INFO", msg);

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(srv + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) continue;

        int client = accept(srv, NULL, NULL);
        if (client < 0) continue;

        pthread_t tid;
        int *cfd = malloc(sizeof(int));
        *cfd = client;
        if (pthread_create(&tid, NULL, handle_client, cfd) != 0) {
            free(cfd); close(client);
        } else {
            pthread_detach(tid);
        }
    }

    log_ts("INFO", "shutting down");
    close(srv);
    unlink(sock_path);
    if (serial_fd >= 0) close(serial_fd);
    return 0;
}
