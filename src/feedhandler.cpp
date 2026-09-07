// Payloads -> OrderMessage -> type, symbol, price, size, timestamp ->
// feedHandler->feedpublisher->subscriber

#include "deepplus.h"
#include "env.h"
#include "feedpublisher.h"
#include "helpers.h"
#include "iextp.h"
#include "net.h"
#include <cstdint>
#include <cstdio>
#include <print>
#include <thread>
struct FeedHandler {
    DeepPlusDecoder decoder;
    FeedPublisher publisher; // the middle link: our own sequenced multicast
    uint64_t expected = 0;
    uint64_t capture_gaps = 0;
    uint64_t published = 0;
    bool warned_protocol = false;

    void on_payload(const uint8_t *payload, size_t length) {
        IextpHeader header;
        if (!iextp_decode(payload, length, header)) {
            return;
        }
        if (header.protocol != 0x8005) {
            if (!warned_protocol) {
                warned_protocol = true;
                fprintf(stderr,
                        "Warning: unexpected protocol 0x%04x; skipping "
                        "those packets\n",
                        header.protocol);
            }
            return;
        }
        if (header.message_count == 0) {
            return;
        }
        uint64_t skip = 0;
        if (expected) {
            if (header.first_sequence_number > expected) {
                capture_gaps += header.first_sequence_number - expected;
            } else if (header.first_sequence_number < expected) {
                skip = expected - header.first_sequence_number;
            }
        }
        iextp_for_each_message(
            payload, length, header,
            [&](const uint8_t *message_bytes, size_t message_length) {
                if (skip) {
                    skip--;
                    return;
                }
                OrderMessage out[2];
                int count = decoder.decode(message_bytes, message_length, out);
                for (int i = 0; i < count; ++i) {
                    publisher.publish(out[i]);
                    ++published;
                }
            });

        if (header.first_sequence_number + header.message_count > expected) {
            expected = header.first_sequence_number + header.message_count;
        }
    }
    void report() const {
        std::print("Capture gaps: {}\n", (unsigned long long)capture_gaps);
        std::print("Unknown references: {}\n", (unsigned long long)decoder.unknown_references);
        std::print("Published: {}\n", (unsigned long long)published);
        uint64_t dropped = 0;
        for (const FeedPublisher::Bucket &bucket : publisher.buckets)
            dropped += bucket.dropped;
        std::print("Dropped on purpose: {} ({}% loss injection)\n",
                   (unsigned long long)dropped, publisher.loss_percent);
        uint64_t crossed = 0;
        for (const FeedPublisher::Bucket &bucket : publisher.buckets)
            crossed += bucket.golden.crossed;
        std::print("Crossed-book updates: {}\n", (unsigned long long)crossed);
    }
};
int main() {
    install_sigint();

    FeedHandler handler;
    handler.publisher.init();
    // The repair server has to be listening before the first datagram goes
    // out: a subscriber notices a gap within milliseconds and will ask for it.
    std::thread(serve_repairs, &handler.publisher).detach();

    // Receive the venue's feed the way a real handler does: off a multicast
    // group, never out of a file. Nothing here knows a capture is involved.
    int fd = udp_recv_socket(IEX_GROUP, IEX_PORT, MCAST_IF, RCVBUF_MB << 20);
    std::print("feedhandler: listening on {}:{}\n", IEX_GROUP, IEX_PORT);
    std::fflush(stdout); // so a waiting feedhandler is visibly waiting

    const uint64_t idle_stop_ns = (uint64_t)FEED_IDLE_STOP_MS * 1000000ull;
    const uint64_t start_timeout_ns =
        (uint64_t)FEED_START_TIMEOUT_MS * 1000000ull;
    const uint64_t started = now_ns();
    uint8_t buf[2048];
    uint64_t packets = 0, last_receive = 0;
    bool never_heard_anything = false;

    while (!g_stop) {
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 100) > 0) {
            // Drain the whole queue per wake: one syscall per datagram is CPU
            // taken from the decode that actually matters.
            for (;;) {
                ssize_t received = recv(fd, buf, sizeof buf, 0);
                if (received < 0) {
                    if (errno == EINTR)
                        continue;
                    break; // EAGAIN: drained
                }
                handler.on_payload(buf, (size_t)received);
                packets++;
                last_receive = now_ns();
            }
        }
        if (last_receive) {
            // Multicast has no EOF, so silence after the feed has started is
            // the only signal that the session is over.
            if (now_ns() - last_receive > idle_stop_ns)
                break;
        } else if (now_ns() - started > start_timeout_ns) {
            // Nothing has EVER arrived. A group keeps no history, so if the
            // sender already finished there is nothing left to collect and
            // waiting is pointless -- and indistinguishable from a hang.
            never_heard_anything = true;
            break;
        }
    }
    close(fd);

    handler.publisher.finish(); // the 'E' terminator every subscriber waits for
    std::print("Packets received: {}\n", (unsigned long long)packets);
    handler.report();

    if (never_heard_anything) {
        std::print("\nfeedhandler: nothing arrived on {}:{} within {} ms.\n",
                   IEX_GROUP, IEX_PORT, FEED_START_TIMEOUT_MS);
        std::print("  Multicast keeps no history: a group is not a queue, and "
                   "nothing is stored\n"
                   "  for receivers that were not already joined. If exchange "
                   "has finished, that\n"
                   "  traffic is gone for good -- start feedhandler FIRST, "
                   "then exchange.\n");
        return 1;
    }
    return g_stop ? 130 : 0;
}
