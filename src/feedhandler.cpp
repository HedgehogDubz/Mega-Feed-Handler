// Payloads -> OrderMessage -> type, symbol, price, size, timestamp ->
// feedHandler->feedpublisher->subscriber

#include "deepplus.h"
#include "helpers.h"
#include "iextp.h"
#include <cstdint>
#include <print>
struct FeedHandler {
    DeepPlusDecoder decoder;
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
                    // publish
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
    }
};
int main() {
    // TODO: replay pcapng -> on_payload -> FeedPublisher
    FeedHandler handler;
    handler.report();
    return 0;
}
