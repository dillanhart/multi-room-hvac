/*
 * net.h - TCP sockets: ask another device for info, and serve our status
 */

#ifndef NET_H
#define NET_H

#include <stddef.h>         // size_t

#include "sensor.h"

/*
 * Connect to ip:port, send msg, and read the reply into reply (always
 * NUL-terminated). Returns the number of bytes read, or -1 on failure.
 */
int net_request(const char *ip, int port, const char *msg,
                char *reply, size_t reply_len);

/*
 * Start listening for status requests on port (all interfaces).
 * Returns the listening socket, or -1 on failure.
 */
int net_listen(int port);

/*
 * Wait up to timeout_ms for clients on listen_fd. Each client that connects
 * gets one line with the latest reading and relay states, then is closed.
 * Use this in place of sleep() in the main loop.
 */
void net_serve(int listen_fd, const struct sensor_result *latest, int timeout_ms);

#endif
