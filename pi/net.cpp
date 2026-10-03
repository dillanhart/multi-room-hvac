/*
 * net.c - TCP sockets: ask another device for info, and serve our status
 *
 * Status line format sent to clients (one line, then the socket closes):
 *   temp=72.4 humidity=41.2 co2=612 valid=1 heat=off cool=on fan=on
 * Try it with:  nc <pi-address> <port>
 */

#include <stdio.h>          // snprintf, perror
#include <string.h>         // strlen
#include <unistd.h>         // close
#include <poll.h>           // poll
#include <time.h>           // clock_gettime
#include <sys/socket.h>     // socket, connect, bind, listen, accept
#include <sys/time.h>       // struct timeval
#include <netinet/in.h>     // sockaddr_in
#include <arpa/inet.h>      // inet_pton, htons

#include "net.h"
#include "relay.h"

// How long a connect/send/recv may block before giving up
#define IO_TIMEOUT_SEC 5

// How long to wait for a client to send its request before replying
#define REQUEST_WAIT_MS 200

// Stop a slow peer from stalling the thermostat loop
static void set_io_timeout(int fd) {
    struct timeval tv = { .tv_sec = IO_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/*
 * net_request - send msg to ip:port and read back the reply.
 *
 * Reads until the other side closes the connection or reply is full.
 * Returns: bytes read (reply is NUL-terminated), or -1 on failure.
 */
int net_request(const char *ip, int port, const char *msg,
                char *reply, size_t reply_len) {
    struct sockaddr_in addr = { 0 };
    size_t total = 0;
    int fd;

    if (reply_len == 0)
        return -1;
    reply[0] = '\0';

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "net_request: bad IP address '%s'\n", ip);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    set_io_timeout(fd);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("connect");
        goto fail;
    }
    if (msg && send(fd, msg, strlen(msg), MSG_NOSIGNAL) < 0) {
        perror("send");
        goto fail;
    }
    shutdown(fd, SHUT_WR);  // tell the other side our request is complete

    // Leave room for the NUL terminator
    while (total < reply_len - 1) {
        ssize_t n = recv(fd, reply + total, reply_len - 1 - total, 0);
        if (n < 0) {
            perror("recv");
            goto fail;
        }
        if (n == 0)
            break;  // other side closed: reply is complete
        total += n;
    }
    reply[total] = '\0';
    close(fd);
    return (int)total;

fail:
    close(fd);
    return -1;
}

/*
 * net_listen - open a TCP socket listening on port.
 * Returns: the listening socket, or -1 on failure (error is printed).
 */
int net_listen(int port) {
    struct sockaddr_in addr = { 0 };
    int one = 1;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    // Lets the program restart right away without "address already in use"
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 4)) {
        perror("listen");
        close(fd);
        return -1;
    }
    printf("Listening for status requests on port %d\n", port);
    return fd;
}

/*
 * Read (and for now ignore) whatever the client sent. Closing a socket with
 * unread data makes the kernel reset the connection, which can cut off the
 * reply. Waits briefly, since clients like nc may send nothing at all.
 */
static void read_request(int client) {
    struct pollfd pfd = { .fd = client, .events = POLLIN };
    char buf[128];

    if (poll(&pfd, 1, REQUEST_WAIT_MS) > 0)
        while (recv(client, buf, sizeof(buf), MSG_DONTWAIT) > 0)
            ;
}

// Send one status line to a connected client
static void send_status(int client, const struct sensor_result *latest) {
    char buf[160];
    int len;

    len = snprintf(buf, sizeof(buf),
                   "temp=%.1f humidity=%.1f co2=%d read_ok=%d heat=%s cool=%s fan=%s\n",
                   latest->temp, latest->hum, latest->CO2, latest->read_ok,
                   relay_get(RELAY_HEAT) ? "on" : "off",
                   relay_get(RELAY_COOL) ? "on" : "off",
                   relay_get(RELAY_FAN) ? "on" : "off");
    if (send(client, buf, len, MSG_NOSIGNAL) < 0)
        perror("send");
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * net_serve - answer status requests for up to timeout_ms, then return.
 *
 * If listen_fd is -1 (listening failed) this just sleeps, so the main loop
 * keeps its timing either way.
 */
void net_serve(int listen_fd, const struct sensor_result *latest, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;

    for (;;) {
        long remaining = deadline - now_ms();
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int client;

        if (remaining <= 0)
            return;
        // poll() ignores a negative fd, so this doubles as a sleep
        if (poll(&pfd, 1, (int)remaining) <= 0 || !(pfd.revents & POLLIN))
            continue;

        client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            perror("accept");
            continue;
        }
        set_io_timeout(client);
        read_request(client);
        send_status(client, latest);
        close(client);
    }
}
