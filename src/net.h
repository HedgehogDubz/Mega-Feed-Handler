#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

[[noreturn]] inline void die(const char *msg) {
    std::perror(msg);
    std::exit(EXIT_FAILURE);
}

inline volatile std::sig_atomic_t g_stop = 0;

// The handler does one async-signal-safe store; loops poll g_stop and unwind
// normally. SIGTERM matters as much as SIGINT: `docker stop` sends SIGTERM,
// and an unhandled signal is *ignored* when the process is PID 1 in a
// container, so the runtime waits out the grace period and then SIGKILLs.
inline void install_signals() {
    struct sigaction sa {};
    sa.sa_handler = [](int) { g_stop = 1; };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: blocking calls return EINTR so we can stop
    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        die("sigaction SIGINT");
    }
    if (sigaction(SIGTERM, &sa, nullptr) < 0) {
        die("sigaction SIGTERM");
    }
}

// trade ts
// sleep until next ts - time, wake up, run cycles repeatedly until its the
// right timestamp

// forward
inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
// yankable
inline uint64_t wall_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
inline void ms_sleep(long long ms) {
    timespec ts{(time_t)(ms / 1000), (long)((ms % 1000) * 1000000LL)};
    nanosleep(&ts, nullptr);
}

// Both sleeps bail on g_stop so a signal during a long replay gap does not
// look like a hang.
inline void sleep_until_ns(uint64_t target_ns) {
    while (!g_stop) {
        uint64_t now = now_ns();
        if (now >= target_ns) {
            return;
        }
        uint64_t sleep_ns = target_ns - now;
        timespec ts;
        ts.tv_sec = (time_t)(sleep_ns / 1000000000ull);
        ts.tv_nsec = (long)(sleep_ns % 1000000000ull);
        nanosleep(&ts, nullptr);
    }
}

inline void sleep_spin_until(uint64_t target_ns) {
    while (!g_stop) {
        uint64_t now = now_ns();
        if (now >= target_ns) {
            return;
        }
        uint64_t sleep_ns = target_ns - now;
        if (sleep_ns > 200000ull) {
            uint64_t nap =
                sleep_ns - 150000ull; // wake ~150 us early, spin the rest
            timespec ts;
            ts.tv_sec = (time_t)(nap / 1000000000ull);
            ts.tv_nsec = (long)(nap % 1000000000ull);
            nanosleep(&ts, nullptr);
        }
    }
}

// inet_addr cannot report failure (0xffffffff is both an error and the
// broadcast address), so every address goes through inet_pton.
inline bool parse_ipv4(const std::string &text, in_addr &out) {
    return inet_pton(AF_INET, text.c_str(), &out) == 1;
}

inline void set_nonblocking(int fd) {
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        die("fcntl");
    }
}

inline int udp_send_socket(const std::string &mcast_if) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        die("socket");
    }

    in_addr interface_addr{};
    if (!parse_ipv4(mcast_if, interface_addr)) {
        std::fprintf(stderr, "udp_send_socket: bad interface address '%s'\n",
                     mcast_if.c_str());
        std::exit(EXIT_FAILURE);
    }

    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &interface_addr,
                   sizeof(interface_addr)) < 0) {
        die("setsockopt IP_MULTICAST_IF");
    }

    unsigned char loop = 1;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        die("setsockopt IP_MULTICAST_LOOP");
    }
    unsigned char ttl = 1;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        die("setsockopt IP_MULTICAST_TTL");
    }

    return fd;
}

inline int udp_recv_socket(const std::string &group, int port,
                           const std::string &mcast_if, int rcvbuf_bytes) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        die("socket");
    }

    int on = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        die("setsockopt SO_REUSEADDR");
    }
#ifdef SO_REUSEPORT
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) < 0) {
        die("setsockopt SO_REUSEPORT");
    }
#endif

    // A silently-failed receive buffer is a silent drop under load, so this is
    // checked and the granted size reported rather than assumed.
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_bytes,
                   sizeof(rcvbuf_bytes)) < 0) {
        die("setsockopt SO_RCVBUF");
    }
    int granted = 0;
    socklen_t granted_len = sizeof(granted);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &granted, &granted_len) == 0 &&
        granted < rcvbuf_bytes) {
        std::fprintf(stderr,
                     "warning: SO_RCVBUF capped at %d bytes (asked %d)\n",
                     granted, rcvbuf_bytes);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        die("bind");
    }

    ip_mreq membership{};
    if (!parse_ipv4(group, membership.imr_multiaddr)) {
        std::fprintf(stderr, "udp_recv_socket: bad group address '%s'\n",
                     group.c_str());
        std::exit(EXIT_FAILURE);
    }
    if (!parse_ipv4(mcast_if, membership.imr_interface)) {
        std::fprintf(stderr, "udp_recv_socket: bad interface address '%s'\n",
                     mcast_if.c_str());
        std::exit(EXIT_FAILURE);
    }

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                   sizeof(membership)) < 0) {
        die("setsockopt IP_ADD_MEMBERSHIP");
    }
    set_nonblocking(fd);

    return fd;
}

//BUCKETS: live at 239.77.7.1, .2, .3, .4

inline std::string group_for(const std::string &base, int id) {
    in_addr addr{};
    if (!parse_ipv4(base, addr)) {
        std::fprintf(stderr, "group_for: bad base address '%s'\n",
                     base.c_str());
        std::exit(EXIT_FAILURE);
    }
    addr.s_addr = htonl(ntohl(addr.s_addr) + (uint32_t)id);
    // inet_ntoa returns a pointer into a static buffer; inet_ntop writes into
    // ours, so this stays safe to call from more than one thread.
    char text[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr, text, sizeof(text))) {
        die("inet_ntop");
    }
    return text;
}

inline int tcp_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket");
    }
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        die("setsockopt SO_REUSEADDR");
    }
#ifdef SO_REUSEPORT
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) < 0) {
        die("setsockopt SO_REUSEPORT");
    }
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        die("bind");
    }
    if (listen(fd, SOMAXCONN) < 0) {
        die("listen");
    }
    return fd;
}

inline int tcp_connect(const std::string &host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (!parse_ipv4(host, addr.sin_addr)) {
        close(fd);
        return -1;
    }
    addr.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#ifdef SO_NOSIGPIPE
    // BSD/macOS only; on Linux the equivalent is MSG_NOSIGNAL per send().
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return fd;
}

// A TCP connection is a pipe of bytes with no message boundaries: it will
// hand you half a message, or two and a half. These two loop until exactly
// n bytes have moved 
inline bool read_exact(int fd, void *buf, size_t remaining) {
    uint8_t *cursor = (uint8_t *)buf;
    while (remaining) {
        ssize_t moved = recv(fd, cursor, remaining, 0);
        if (moved == 0)
            return false; // peer closed
        if (moved < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        cursor += moved;
        remaining -= (size_t)moved;
    }
    return true;
}
inline bool write_all(int fd, const void *buf, size_t remaining) {
    const uint8_t *cursor = (const uint8_t *)buf;
    while (remaining) {
        ssize_t moved = send(fd, cursor, remaining, 0);
        if (moved < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return false;
        }
        cursor += moved;
        remaining -= (size_t)moved;
    }
    return true;
}
