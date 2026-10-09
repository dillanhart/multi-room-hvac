// Tests pi/net.cpp over localhost (127.0.0.1): real sockets, no network needed.

#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>         // close
#include <netinet/in.h>     // sockaddr_in
#include <sys/socket.h>     // getsockname

#include "net.h"
#include "relay.h"
#include "fake_relay.h"

// net_listen(0) lets the kernel pick a free port; this asks which one it got
static int port_of(int fd) {
    sockaddr_in addr = {};
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

// a port with nothing listening on it: bind one, then close it
static int unused_port() {
    int fd = net_listen(0);
    int port = port_of(fd);
    close(fd);
    return port;
}

class StatusServer : public ::testing::Test {
protected:
    const int SERVE_MS = 500;
    sensor_result latest = { .temp = 70.5f, .hum = 41.2f, .CO2 = 612, .read_ok = 1 };
    int listen_fd = -1;
    int port = 0;

    void SetUp() override {
        fake_relay_reset();
        listen_fd = net_listen(0);
        ASSERT_GE(listen_fd, 0);
        port = port_of(listen_fd);
    }
    void TearDown() override { close(listen_fd); }

    // net_serve blocks for SERVE_MS, so it runs on a second thread while the
    // test thread acts as the client
    std::string request(const char *msg, size_t reply_len = 256, int *result = nullptr) {
        std::thread server([this] { net_serve(listen_fd, &latest, SERVE_MS); });
        std::string reply(reply_len, 'x');  // filled with junk to catch a missing NUL
        int n = net_request("127.0.0.1", port, msg, reply.data(), reply_len);
        server.join();
        if (result) *result = n;
        return n < 0 ? "" : std::string(reply.c_str());
    }
};

TEST_F(StatusServer, RepliesWithReadingAndRelays) {
    relay_set(RELAY_COOL, 1);
    relay_set(RELAY_FAN, 1);
    int n = 0;

    std::string reply = request("status\n", 256, &n);
    EXPECT_EQ(reply, "temp=70.5 humidity=41.2 co2=612 read_ok=1 heat=off cool=on fan=on\n");
    EXPECT_EQ(n, (int)reply.size());
}

TEST_F(StatusServer, RepliesWhenClientSendsNothing) {
    // like `nc <pi> <port>`: connects and just waits
    EXPECT_EQ(request(nullptr).rfind("temp=70.5 ", 0), 0u);
}

TEST_F(StatusServer, ShortReplyBufferIsTruncatedAndTerminated) {
    int n = 0;
    std::string reply = request("status\n", 10, &n);

    EXPECT_EQ(n, 9);  // one byte kept for the NUL
    EXPECT_EQ(reply, "temp=70.5");
}

TEST(NetServe, WithoutSocketJustWaits) {
    sensor_result latest = {};
    auto start = std::chrono::steady_clock::now();

    net_serve(-1, &latest, 200);  // listening failed: the loop still keeps its timing
    auto waited = std::chrono::steady_clock::now() - start;
    EXPECT_GE(waited, std::chrono::milliseconds(200));
    EXPECT_LT(waited, std::chrono::milliseconds(1000));
}

TEST(NetRequest, ConnectionRefusedFails) {
    char reply[64] = "junk";
    EXPECT_EQ(net_request("127.0.0.1", unused_port(), "status\n", reply, sizeof(reply)), -1);
    EXPECT_STREQ(reply, "");
}

TEST(NetRequest, BadAddressFails) {
    char reply[64] = "junk";
    EXPECT_EQ(net_request("not-an-ip", 80, "status\n", reply, sizeof(reply)), -1);
    EXPECT_STREQ(reply, "");
}

TEST(NetRequest, ZeroLengthBufferFails) {
    char reply[1] = { 'x' };
    EXPECT_EQ(net_request("127.0.0.1", 80, "status\n", reply, 0), -1);
    EXPECT_EQ(reply[0], 'x') << "nothing may be written to a zero-length buffer";
}
