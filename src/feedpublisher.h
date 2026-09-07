#pragma once
// feedpublisher.h -- the sending side of the reliable multicast: republish
// neutral Msgs on OUR multicast, split into buckets by symbol, with sequence
// numbers, a ring buffer, and a TCP repair server -- so any number of
// consumers each get every message exactly once, in order, joining only the
// buckets they care about. subscriber.h is the receiving side.

#include "book.h"
#include "env.h"
#include "helpers.h"
#include "net.h"
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <string>
struct FeedPublisher {
    int bucket_count = 0, loss_percent = 0, repair_port = 0, send_fd = -1;
    size_t ring_size = 0;
    std::mt19937_64 loss_rng;

    struct Slot {
        uint64_t sequence = 0;
        bool used = false;
        uint8_t msg[ORDER_MESSAGE_SIZE];
    };
    struct Bucket {
        uint64_t next_sequence = 1, published = 0, dropped = 0;
        std::vector<Slot> ring; // the sender's memory: the last RING_SIZE
                                // messages, so repair requests can be answered
        sockaddr_in destination{};
        BookSet golden; // fed every message before loss: the reference
    };
    std::vector<Bucket> buckets;
    std::mutex ring_mutex; // the ring is shared with the repair thread

    void init() {
        bucket_count = BUCKETS;
        ring_size = RING_SIZE;
        loss_percent = LOSS_PERCENT * 100;
        repair_port = RETRANS_PORT;
        loss_rng.seed(SEED ^ 0x10557ull);
        send_fd = udp_send_socket(MCAST_IF);
        std::string group_base = GROUP_BASE;
        int port_base = DATA_PORT_BASE;
        buckets.resize((size_t)bucket_count);
        for (int i = 0; i < bucket_count; i++) {
            buckets[(size_t)i].ring.resize(ring_size);
            buckets[(size_t)i].destination.sin_family = AF_INET;
            buckets[(size_t)i].destination.sin_port =
                htons((uint16_t)(port_base + i));
            if (!parse_ipv4(group_for(group_base, i),
                            buckets[(size_t)i].destination.sin_addr)) {
                std::fprintf(stderr,
                             "FeedPublisher: bad group address for bucket %d\n",
                             i);
                std::exit(EXIT_FAILURE);
            }
        }
    }

    // One send, however many listeners: the network duplicates it to everyone
    // who joined the group. Ten consumers cost the same as one, and nobody is
    // "first in the queue" -- which is exactly why exchanges publish this way.
    void publish(const OrderMessage &msg) {
        int bucket_index = bucket_of(msg.symbol, bucket_count);
        Bucket &bucket = buckets[(size_t)bucket_index];
        bucket.golden.apply(msg);

        uint8_t payload[ORDER_MESSAGE_SIZE] = {};
        OrderMessage_encode(payload, msg);
        uint64_t sequence;
        {
            std::lock_guard<std::mutex> lock(ring_mutex);
            sequence = bucket.next_sequence++;
            Slot &slot =
                bucket
                    .ring[sequence % ring_size]; // old entries are overwritten
            slot.sequence = sequence;
            slot.used = true;
            memcpy(slot.msg, payload, ORDER_MESSAGE_SIZE);
        }
        bucket.published++;

        // Deliberate loss, injected AFTER the ring is written: the datagram
        // vanishes from the network but the repair path can still serve it.

        if (loss_percent && (int)(loss_rng() % 100) < loss_percent) {
            bucket.dropped++;
            return;
        }

        uint8_t datagram[HDR_BYTES + ORDER_MESSAGE_SIZE];
        size_t datagram_length =
            dgram_encode(datagram, (uint16_t)bucket_index, sequence, payload);
        sendto(send_fd, datagram, datagram_length, 0,
               (sockaddr *)&bucket.destination, sizeof bucket.destination);
    }

    // END sent twenty times over half a second,

    void finish() {
        OrderMessage end_message;
        end_message.type = 'E';
        uint8_t payload[ORDER_MESSAGE_SIZE] = {};
        OrderMessage_encode(payload, end_message);
        std::vector<uint64_t> end_sequences((size_t)bucket_count);
        {
            std::lock_guard<std::mutex> lock(ring_mutex);
            for (int i = 0; i < bucket_count; i++) {
                Bucket &bucket = buckets[(size_t)i];
                end_sequences[(size_t)i] = bucket.next_sequence++;
                Slot &slot = bucket.ring[end_sequences[(size_t)i] % ring_size];
                slot.sequence = end_sequences[(size_t)i];
                slot.used = true;
                memcpy(slot.msg, payload, ORDER_MESSAGE_SIZE);
            }
        }
        for (int round = 0; round < 20; round++) {
            for (int i = 0; i < bucket_count; i++) {
                if (loss_percent && (int)(loss_rng() % 100) < loss_percent)
                    continue; // even endings get lost
                uint8_t datagram[HDR_BYTES + ORDER_MESSAGE_SIZE];
                size_t datagram_length = dgram_encode(
                    datagram, (uint16_t)i, end_sequences[(size_t)i], payload);
                sendto(send_fd, datagram, datagram_length, 0,
                       (sockaddr *)&buckets[(size_t)i].destination,
                       sizeof buckets[(size_t)i].destination);
            }
            ms_sleep(25);
        }
    }
};

// ---- the TCP repair server -------------------------------------------------
// Consumers connect here and ask for exact sequence numbers. Answers come
// straight out of the ring; anything already overwritten is answered with an
inline void serve_one_client(FeedPublisher *publisher, int client_fd) {
    uint8_t request_header[4];
    while (read_exact(client_fd, request_header, 4)) {
        uint16_t bucket_index = get_u16(request_header),
                 count = get_u16(request_header + 2);
        if ((int)bucket_index >= publisher->bucket_count || count == 0 ||
            count > MAX_REPAIR_BATCH)
            break;
        std::vector<uint8_t> sequence_bytes((size_t)count * 8);
        if (!read_exact(client_fd, sequence_bytes.data(),
                        sequence_bytes.size()))
            break;
        std::vector<uint8_t> response;
        {
            std::lock_guard<std::mutex> lock(publisher->ring_mutex);
            auto &ring = publisher->buckets[bucket_index].ring;
            for (uint32_t i = 0; i < count; i++) {
                uint64_t sequence = get_u64(&sequence_bytes[(size_t)i * 8]);
                const FeedPublisher::Slot &slot =
                    ring[sequence % publisher->ring_size];
                uint8_t frame_header[12];
                put_u16(frame_header, bucket_index);
                put_u64(frame_header + 2, sequence);
                bool in_ring = slot.used && slot.sequence == sequence;
                put_u16(frame_header + 10, in_ring ? (uint16_t)ORDER_MESSAGE_SIZE : 0);
                response.insert(response.end(), frame_header,
                                frame_header + 12);
                if (in_ring)
                    response.insert(response.end(), slot.msg,
                                    slot.msg + ORDER_MESSAGE_SIZE);
            }
        }
        if (!write_all(client_fd, response.data(), response.size()))
            break;
    }
    close(client_fd);
}

inline void serve_repairs(FeedPublisher *publisher) {
    int listen_fd = tcp_listen(publisher->repair_port);
    for (;;) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0)
            continue;
        int on = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &on,
                   sizeof on); // repairs are urgent
#ifdef SO_NOSIGPIPE
        setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
        std::thread(serve_one_client, publisher, client_fd).detach();
    }
}
