// exchange -- the fake exchange. Replays a capture onto multicast as the
// venue's own IEX-TP feed, byte for byte, and nothing more: no sequencing of
// our own, no buckets, no repair. A feed handler on the other end cannot tell
// this from the real wire, which is the entire point.
//
// exchange -> feedhandler -> subscriber

#include "env.h"
#include "helpers.h"
#include "net.h"
#include "pcapng_reader.h"
#include <cstdio>
#include <vector>

int main() {
    install_sigint();

    PcapngReader reader;
    if (!reader.open(PCAP_FILE)) {
        return 1;
    }

    int fd = udp_send_socket(MCAST_IF);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons((uint16_t)IEX_PORT);
    if (!parse_ipv4(IEX_GROUP, destination.sin_addr)) {
        std::fprintf(stderr, "exchange: bad group address '%s'\n", IEX_GROUP);
        return 1;
    }
    std::printf("exchange: replaying %s -> %s:%d\n", PCAP_FILE, IEX_GROUP,
                IEX_PORT);

    // Each captured UDP payload IS an exchange datagram; it goes out exactly
    // as recorded. Send failures are counted rather than fatal: a full socket
    // buffer is the network dropping a packet, which is a fact of multicast
    // and precisely what the feed handler's gap detection exists to notice.
    std::vector<uint8_t> payload;
    uint64_t timestamp_ns = 0, packets = 0, bytes = 0, send_failures = 0;
    const uint64_t started = now_ns();
    while (!g_stop && packets < PCAP_PACKET_COUNT &&
           reader.next_packet(payload, timestamp_ns)) {
        if (sendto(fd, payload.data(), payload.size(), 0,
                   (sockaddr *)&destination, sizeof destination) < 0) {
            send_failures++;
        }
        packets++;
        bytes += payload.size();

        // Pace in blocks rather than per packet: one clock read and at most
        // one sleep per thousand sends, instead of two syscalls on every one.
        if (REPLAY_PPS && packets % 1000 == 0) {
            uint64_t due =
                started + (uint64_t)((double)packets / (double)REPLAY_PPS * 1e9);
            uint64_t now = now_ns();
            if (due > now)
                ms_sleep((long long)((due - now) / 1000000ull));
        }
    }
    const double elapsed = (double)(now_ns() - started) / 1e9;

    std::printf("exchange: sent %llu packets (%llu bytes) in %.1fs = %.0f pkt/s,"
                " %llu send failures\n",
                (unsigned long long)packets, (unsigned long long)bytes, elapsed,
                elapsed > 0 ? (double)packets / elapsed : 0.0,
                (unsigned long long)send_failures);
    close(fd);
    return g_stop ? 130 : 0;
}
