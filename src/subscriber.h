#pragma once

#include "env.h"
#include "helpers.h"
#include "net.h"
#include <cerrno>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <vector>

struct SubscriberStats {
    uint64_t delivered = 0, repaired = 0, unrecoverable = 0, duplicates = 0,
             repair_requests = 0;
};

class Subscriber {
  private:
    struct Hole {
        uint64_t noticed = 0, asked = 0;
    };
    struct Bucket {
        int id = 0, udp_fd = -1;
        bool synced = false, done = false;
        uint64_t next_sequence = 1;
        std::map<uint64_t, OrderMessage>
            parked;              // the parking area: early arrivals
        std::set<uint64_t> gone; // sender said "overwritten": unrecoverable
        std::map<uint64_t, Hole>
            holes; // known-missing, with when we noticed/asked
        SubscriberStats stats;
    };

    // TCP is a byte pipe; frames reassemble here. Sized by construction rather
    // than grown on demand: the largest response anyone can legitimately owe us
    // is every bucket answering a full batch at once, plus one recv() of
    // overshoot. A peer sending more than that is misbehaving, and the parser
    // resyncs instead of allocating without limit.
    static constexpr size_t REPAIR_FRAME_MAX = 12 + ORDER_MESSAGE_SIZE;
    static constexpr size_t REPAIR_BUF_CAP =
        MAX_REPAIR_BATCH * REPAIR_FRAME_MAX * BUCKETS + 4096;

    std::vector<Bucket> buckets_;
    int repair_fd_ = -1; // one repair connection serves all buckets
    uint8_t repair_buffer_[REPAIR_BUF_CAP];
    size_t repair_len_ = 0; // bytes held, always < REPAIR_BUF_CAP
    std::string repair_host_;
    int repair_port_ = 0;
    uint64_t nak_delay_ns_ = 0, nak_repeat_ns_ = 0, repair_retry_ns_ = 0,
             giveup_ns_ = 0;
    uint64_t repair_blocked_until_ = 0; // no reconnect attempts before this
    uint64_t backfill_ = 0; // how much recent history to pull when joining live
    uint64_t datagrams_seen_ = 0; // has the feed ever spoken to us at all?
    bool warned_silent_ = false;  // the "still waiting" note is printed once

    // Drop the repair connection AND the half-frame sitting in the reassembly
    // buffer. Keeping those bytes would prepend them to the next connection's
    // stream, misaligning every frame header from then on -- and since a bogus
    // length advances the read cursor by the wrong amount, it never resyncs.
    void reset_repair() {
        if (repair_fd_ >= 0)
            close(repair_fd_);
        repair_fd_ = -1;
        repair_len_ = 0;
    }

    Bucket *find_bucket(uint16_t id) {
        for (Bucket &bucket : buckets_)
            if (bucket.id == (int)id)
                return &bucket;
        return nullptr;
    }

    void drain_udp(Bucket &bucket) {
        // Take everything queued on each wake, not one datagram per system
        // call -- at tens of thousands of messages a second, one syscall per
        // message is real CPU taken from everyone else on the machine.
        uint8_t buf[512];
        for (;;) {
            ssize_t received = recv(bucket.udp_fd, buf, sizeof buf, 0);
            if (received < 0) {
                if (errno == EINTR)
                    continue; // a signal, not an empty queue: keep draining
                break; // EAGAIN/EWOULDBLOCK: drained (the socket is nonblocking)
            }
            Dgram datagram;
            if (!dgram_decode(buf, (size_t)received, datagram) ||
                (int)datagram.bucket != bucket.id)
                continue;
            datagrams_seen_++;
            accept_OrderMessage(bucket, datagram.sequence, datagram.message);
        }
    }

    void accept_OrderMessage(Bucket &bucket, uint64_t sequence,
                             const OrderMessage &msg) {
        if (!bucket.synced) {
            // Tuned in mid-stream. Rewinding the start point turns the gap
            // detection just below into a bounded backfill: start..sequence-1
            // become ordinary holes and the existing NAK path fetches them.
            // That fills the book without replaying the entire session.
            uint64_t start = sequence;
            if (backfill_)
                start = sequence > backfill_ ? sequence - backfill_ : 1;
            bucket.next_sequence = start;
            bucket.synced = true;
        }
        if (sequence < bucket.next_sequence || bucket.parked.count(sequence)) {
            bucket.stats.duplicates++;
            return;
        }
        // Every number between what we expected and what just arrived is now a
        // known hole -- noticed immediately, with no help from anyone. A lost
        // datagram leaves no other trace.
        uint64_t now = now_ns();
        for (uint64_t missing = bucket.next_sequence; missing < sequence;
             missing++)
            if (!bucket.parked.count(missing) && !bucket.holes.count(missing) &&
                !bucket.gone.count(missing))
                bucket.holes[missing] = Hole{now, 0};
        bucket.holes.erase(sequence);
        bucket.parked[sequence] =
            msg; // parked: not delivered (order!), not dropped (loss!)
    }

    void nak(Bucket &bucket) {
        if (bucket.holes.empty())
            return;
        uint64_t now = now_ns();

        // The one and only place data is written off. A hole we have chased for
        // REPAIR_GIVEUP_MS is accepted as lost, which lets next_sequence move
        // past it instead of stalling the bucket forever. Nothing else may
        // declare a message unrecoverable -- in particular a failed connect
        // must not, because the sender's ring almost certainly still has it.
        //
        // The expired holes are always a PREFIX of the map: holes are keyed by
        // sequence, and next_sequence only moves forward, so a hole's
        // `noticed` never decreases as its sequence rises. Stopping at the
        // first live one keeps this O(expired) rather than O(all holes) --
        // which matters enormously with a large backfill, where walking every
        // hole on each 2 ms tick burns a whole core.
        for (auto it = bucket.holes.begin(); it != bucket.holes.end();) {
            if (now - it->second.noticed > giveup_ns_) {
                bucket.gone.insert(it->first);
                it = bucket.holes.erase(it);
            } else {
                break;
            }
        }
        if (bucket.holes.empty())
            return;

        // Backing off after a failed attempt: nothing can go out, so do not
        // build a batch that would only be discarded. Checked BEFORE the scan
        // below, not after it.
        if (repair_fd_ < 0 && now < repair_blocked_until_)
            return;

        std::vector<uint64_t> to_request;
        to_request.reserve(MAX_REPAIR_BATCH);
        for (auto &[sequence, hole] : bucket.holes) {
            // Wait NAK_DELAY before asking: packets that merely arrived out of
            // order show up on their own a hair later, and the delay lets
            // several holes collect into one request. Re-ask after
            // NAK_REPEAT_MS in case the repair itself was lost.
            bool first_request =
                hole.asked == 0 && now - hole.noticed > nak_delay_ns_;
            bool re_request =
                hole.asked != 0 && now - hole.asked > nak_repeat_ns_;
            if (first_request || re_request)
                to_request.push_back(sequence);
            // holes is ordered by sequence, so a full batch already holds the
            // oldest gaps -- the rest can wait for the next call rather than
            // rescanning the whole map to reject them.
            if (to_request.size() == MAX_REPAIR_BATCH)
                break;
        }
        if (to_request.empty())
            return;

        if (repair_fd_ < 0) {
            repair_fd_ = tcp_connect(repair_host_, repair_port_);
            if (repair_fd_ < 0) {
                // Keep every hole. The server may be restarting, and the data
                // is probably still in its ring -- so back off and try again
                // rather than destroying messages we could still recover.
                repair_blocked_until_ = now + repair_retry_ns_;
                return;
            }
            set_nonblocking(repair_fd_);
        }
        // Ask for exactly the numbers that are missing, never a span.
        std::vector<uint8_t> request(4 + 8 * to_request.size());
        put_u16(request.data(), (uint16_t)bucket.id);
        put_u16(request.data() + 2, (uint16_t)to_request.size());
        for (size_t i = 0; i < to_request.size(); i++)
            put_u64(request.data() + 4 + 8 * i, to_request[i]);
        if (!write_all(repair_fd_, request.data(), request.size())) {
            reset_repair();
            repair_blocked_until_ = now + repair_retry_ns_;
            return; // holes keep asked==0, so they re-ask once we reconnect
        }
        // Stamped only now that the bytes are away. Stamping while building the
        // batch would silence these holes for a full repeat interval even
        // though the connect or the write above failed and nothing went out.
        for (uint64_t sequence : to_request) {
            auto it = bucket.holes.find(sequence);
            if (it != bucket.holes.end())
                it->second.asked = now;
        }
        bucket.stats.repair_requests++;
    }

    void drain_tcp() {
        for (;;) {
            if (repair_len_ == REPAIR_BUF_CAP)
                break; // full: parse what we have, resume on the next call
            ssize_t received = recv(repair_fd_, repair_buffer_ + repair_len_,
                                    REPAIR_BUF_CAP - repair_len_, 0);
            if (received == 0) { // server closed
                reset_repair();
                return; // nothing buffered is still trustworthy
            }
            if (received < 0) {
                if (errno == EINTR)
                    continue; // a signal, not an empty socket
                break;        // EAGAIN/EWOULDBLOCK: drained
            }
            repair_len_ += (size_t)received;
        }
        // Frames: [bucket u16][seq u64][len u16][len bytes]. The buffer may
        // hold half a frame -- that half waits here for the rest.
        size_t offset = 0;
        while (repair_len_ - offset >= 12) {
            const uint8_t *frame = repair_buffer_ + offset;
            uint16_t bucket_id = get_u16(frame);
            uint64_t sequence = get_u64(frame + 2);
            uint16_t length = get_u16(frame + 10);
            if (repair_len_ - offset < 12 + (size_t)length)
                break;
            // A length that is neither a payload nor a "gone" marker means the
            // framing is wrong. Erasing the hole here would be fatal: nothing
            // would park it and nothing would mark it gone, so next_sequence
            // could never advance past it and the bucket would stall for good.
            // Leave the holes alone -- the NAK timer re-asks -- and resync.
            if (length != 0 && length != ORDER_MESSAGE_SIZE) {
                reset_repair();
                return;
            }
            Bucket *bucket = find_bucket(bucket_id);
            if (bucket) {
                bucket->holes.erase(sequence);
                if (length == 0) {
                    // The sender's ring no longer has it. Recorded honestly as
                    // unrecoverable -- never silently skipped.
                    if (sequence >= bucket->next_sequence)
                        bucket->gone.insert(sequence);
                } else if (sequence >= bucket->next_sequence &&
                           !bucket->parked.count(sequence)) {
                    bucket->parked[sequence] = OrderMessage_decode(frame + 12);
                    bucket->stats.repaired++;
                }
            }
            offset += 12 + length;
        }
        // Shift the unparsed tail (at most one partial frame in the common
        // case) to the front, so the next recv has the whole buffer again.
        repair_len_ -= offset;
        if (offset && repair_len_)
            memmove(repair_buffer_, repair_buffer_ + offset, repair_len_);
    }

    void deliver(
        Bucket &bucket,
        const std::function<void(int, const OrderMessage &)> &on_OrderMessage) {
        // Release everything contiguous, in order. When a hole is filled,
        // the whole queue waiting behind it goes out at once.
        while (!bucket.done) {
            auto it = bucket.parked.find(bucket.next_sequence);
            if (it != bucket.parked.end()) {
                OrderMessage msg = it->second;
                bucket.parked.erase(it);
                bucket.next_sequence++;
                if (msg.type == 'E') {
                    bucket.done = true;
                    break;
                }
                bucket.stats.delivered++;
                on_OrderMessage(bucket.id, msg);
            } else if (bucket.gone.count(bucket.next_sequence)) {
                bucket.gone.erase(bucket.next_sequence);
                bucket.next_sequence++;
                bucket.stats.unrecoverable++;
            } else
                break;
        }
    }

  public:
    // first_expected is per-consumer policy, not a global setting:
    //   0 = tune in live, taking the stream from wherever it happens to be.
    //   N = demand everything from sequence N, repairing backwards over TCP.
    // A live view wants 0; something reconciling or auditing the whole session
    // wants 1. Defaults to EXPECT_FROM so existing callers are unchanged.
    // backfill applies only when first_expected is 0 (joining live): on the
    // first datagram, rewind this many sequences and repair them, so the book
    // is not full of levels that were set before we arrived. Bounded by
    // construction, unlike replaying from sequence 1 -- and capped in practice
    // by the sender's ring, which answers anything older with "gone".
    explicit Subscriber(const std::vector<int> &bucket_ids,
                        uint64_t first_expected = EXPECT_FROM,
                        uint64_t backfill = 0) {
        backfill_ = first_expected ? 0 : backfill;
        std::string group_base = GROUP_BASE;
        std::string mcast_if = MCAST_IF;
        int port_base = DATA_PORT_BASE;
        int receive_buffer_bytes = RCVBUF_MB << 20;
        repair_host_ = RETRANS_HOST;
        repair_port_ = RETRANS_PORT;
        nak_delay_ns_ = (uint64_t)NAK_DELAY_MS * 1000000ull;
        // (first_expected arrives as a parameter now, not read from env.h)
        nak_repeat_ns_ = (uint64_t)NAK_REPEAT_MS * 1000000ull;
        repair_retry_ns_ = (uint64_t)REPAIR_RETRY_MS * 1000000ull;
        giveup_ns_ = (uint64_t)REPAIR_GIVEUP_MS * 1000000ull;

        for (int id : bucket_ids) {
            Bucket bucket;
            bucket.id = id;
            // Each bucket is its own multicast group and port: the kernel
            // filters out buckets we didn't join before this program ever
            // sees a byte of them. That IS the bucketing feature.
            bucket.udp_fd =
                udp_recv_socket(group_for(group_base, id), port_base + id,
                                mcast_if, receive_buffer_bytes);
            if (first_expected > 0) { // demand history via repair
                bucket.next_sequence = first_expected;
                bucket.synced = true;
            }
            buckets_.push_back(std::move(bucket));
        }
    }

    ~Subscriber() {
        for (Bucket &bucket : buckets_)
            if (bucket.udp_fd >= 0)
                close(bucket.udp_fd);
        reset_repair();
    }

    // Raw descriptors carry no shared ownership: a copy would close every
    // socket twice and leave one of the two reading a dead fd.
    Subscriber(const Subscriber &) = delete;
    Subscriber &operator=(const Subscriber &) = delete;

  
    void
    pump(const std::function<void(int, const OrderMessage &)> &on_OrderMessage) {
        for (Bucket &bucket : buckets_)
            drain_udp(bucket);
        if (repair_fd_ >= 0)
            drain_tcp();
        for (Bucket &bucket : buckets_) {
            nak(bucket);
            deliver(bucket, on_OrderMessage);
        }
    }

    // True once every joined bucket has reached its 'E' terminator.
    bool done() const {
        for (const Bucket &bucket : buckets_)
            if (!bucket.done)
                return false;
        return true;
    }

    SubscriberStats stats() const {
        SubscriberStats total;
        for (const Bucket &bucket : buckets_) {
            total.delivered += bucket.stats.delivered;
            total.repaired += bucket.stats.repaired;
            total.unrecoverable += bucket.stats.unrecoverable;
            total.duplicates += bucket.stats.duplicates;
            total.repair_requests += bucket.stats.repair_requests;
        }
        return total;
    }

    // Per-bucket view, for reporting which stream lost what.
    SubscriberStats stats_for(int bucket_id) const {
        for (const Bucket &bucket : buckets_)
            if (bucket.id == bucket_id)
                return bucket.stats;
        return SubscriberStats{};
    }
    // True once at least one datagram has arrived on any joined bucket.
    bool heard_feed() const { return datagrams_seen_ > 0; }

    // Blocking driver: sleep until a socket has something or the timeout
    // fires, then pump() once. Returns true if every bucket reached its 'E'
    // terminator; false if interrupted, or if the feed never spoke at all --
    // multicast keeps no history, so a publisher that already finished leaves
    // nothing behind, and waiting on it forever just looks like a hang.
    bool run(const std::function<void(int, const OrderMessage &)> &on_msg) {
        // Built once, not per iteration: the bucket descriptors never change,
        // and only the repair socket comes and goes -- so it gets the last
        // slot and is simply excluded from the count while disconnected.
        std::vector<pollfd> pfds(buckets_.size() + 1);
        for (size_t i = 0; i < buckets_.size(); i++)
            pfds[i] = {buckets_[i].udp_fd, POLLIN, 0};
        const size_t repair_slot = buckets_.size();
        const uint64_t started = now_ns();

        while (!g_stop) {
            pfds[repair_slot] = {repair_fd_, POLLIN, 0};
            nfds_t count = (nfds_t)(repair_fd_ >= 0 ? repair_slot + 1
                                                    : repair_slot);
            // The 2 ms timeout matters: a loop that only wakes when data
            // arrives can never notice a gap sitting unrepaired during a
            // quiet spell.
            poll(pfds.data(), count, 2);
            pump(on_msg);
            if (done())
                return true;
            // Waiting for a publisher that has not started yet is a perfectly
            // normal thing to do, so this never gives up. But silence that
            // lasts is indistinguishable from a crash, so say once what the
            // likely cause is and then carry on waiting.
            if (!datagrams_seen_ && !warned_silent_ &&
                now_ns() - started >
                    (uint64_t)SUB_START_TIMEOUT_MS * 1000000ull) {
                warned_silent_ = true;
                std::fprintf(stderr,
                             "subscriber: nothing on any joined bucket after "
                             "%d ms -- still waiting.\n"
                             "  If the publisher already finished, there is "
                             "nothing left to receive:\n"
                             "  multicast keeps no history for receivers that "
                             "were not joined.\n",
                             SUB_START_TIMEOUT_MS);
            }
        }
        return false;
    }
};
