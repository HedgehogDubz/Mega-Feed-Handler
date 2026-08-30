#pragma once
#include <_time.h>
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

[[noreturn]] void die(const char *msg) {
    std::perror(msg);
    std::exit(EXIT_FAILURE);
}

inline volatile sig_atomic_t g_stop = 0;
inline void install_sigint() {
    signal(SIGINT, [](int) {
        std::printf("SIGINT received, exiting...\n");
        std::exit(EXIT_SUCCESS);
    });
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

inline void sleep_until_ns(uint64_t target_ns) {
    while (true) {
        uint64_t now = now_ns();
        if (now >= target_ns) {
            return;
        }
        uint64_t sleep_ns = target_ns - now;
        timespec ts;
        ts.tv_sec = sleep_ns / 1000000000ull;
        ts.tv_nsec = sleep_ns % 1000000000ull;
        nanosleep(&ts, nullptr);
    }
}

inline void sleep_spin_until(uint64_t target_ns) {
    for (;;) {
        uint64_t now = now_ns();
        if (now >= target_ns) {
            return;
        }
        uint64_t sleep_ns = target_ns - now;
        if (sleep_ns > 200000ull) {
            timespec ts{0, (long)(sleep_ns - 150000)};
            nanosleep(&ts, nullptr);
        }
    }
}

