#pragma once
#include <cstdint>
#include <cstddef>
inline constexpr const char *PCAP_FILE = "pcapngs/20241206_IEXTP1_DPLS1.0.pcap";

inline constexpr const char *MCAST_IF = "127.0.0.1";

// The exchange's own feed: exchange replays the capture here, feedhandler
// joins it. This MUST NOT collide with GROUP_BASE/DATA_PORT_BASE below --
// those carry our republished, bucketed feed. Sharing an address would put
// raw IEX packets on bucket 0's group, where they fail the MAGIC check and
// are dropped without a trace, and would feed our own output back to us.
inline constexpr const char *IEX_GROUP = "239.77.6.1";
inline constexpr int IEX_PORT = 1233;

inline constexpr uint64_t PCAP_PACKET_COUNT = 1000000;

// Replay rate, packets per second (0 = as fast as the disk allows).
// This is not a nicety: unpaced, exchange empties the capture at disk speed
// and simply overruns the feed handler -- roughly half the packets never
// arrive, and a DEEP+ stream with half its messages missing produces crossed
// books and unresolvable order references. A real venue spreads the same
// traffic across a trading day.
inline constexpr uint64_t REPLAY_PPS = 50000;

inline constexpr int BUCKETS = 4;
inline constexpr size_t RING_SIZE = 262144;
inline constexpr double LOSS_PERCENT = 0.05;

inline constexpr uint64_t SEED = 42;
//Repair TCP port:
inline constexpr const char *RETRANS_HOST = "127.0.0.1";
inline constexpr int RETRANS_PORT = 7799;
//Starting multicast port for the buckets (goes ...7.1, ...7.2, ...7.3, ...7.4)
inline constexpr const char *GROUP_BASE = "239.77.7.1";
// where multicaast UDP port starts for the buckets (goes 1234, 1235, 1236, 1237)
inline constexpr int DATA_PORT_BASE = 1234;





//Subscriber
// 0 = accept stream from wherever it happens to be when we tune in. 1 = repairs backwards over TCP for everything it missed.
inline constexpr uint64_t EXPECT_FROM = 1;
//Kernel allocates how much Megabytes per Multicast Receive buffer
inline constexpr int RCVBUF_MB = 8;
//how long one should wait after receiving an error to send tcp correction (to batch multiple together)
inline constexpr int NAK_DELAY_MS = 5;
//re-ask for a hole whose earlier request went unanswered after this long
inline constexpr int NAK_REPEAT_MS = 200;
//after a failed connect to the repair server, wait this long before trying
//again -- without it a 2 ms poll loop hammers connect() hundreds of times a second
inline constexpr int REPAIR_RETRY_MS = 100;
//How long to wait for the feed to start at all before giving up. Distinct from
//FEED_IDLE_STOP_MS below, which only applies once data HAS arrived: a group
//that never carries a packet would otherwise be waited on forever, which just
//looks like a hang.
inline constexpr int FEED_START_TIMEOUT_MS = 10000;
//Same idea one layer down, for a subscriber: if no datagram has EVER arrived
//on its buckets after this long, the publisher is not running (or already
//finished) and waiting forever is indistinguishable from a hang.
inline constexpr int SUB_START_TIMEOUT_MS = 10000;
//A multicast group has no end-of-file. Once the exchange feed has started,
//this much silence is taken to mean the session is over.
inline constexpr int FEED_IDLE_STOP_MS = 2000;
//how long a hole may stay unfilled before we accept it is lost for good.
//This is the ONLY thing that writes data off: an unreachable repair server
//must not, because the sender's ring very likely still holds the message.
inline constexpr int REPAIR_GIVEUP_MS = 2000;
