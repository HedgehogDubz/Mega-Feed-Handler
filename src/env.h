#pragma once
#include <cstdint>
#include <cstddef>
constexpr const char * PCAP_FILE = "pcapngs/20241206_IEXTP1_DPLS1.0.pcap";

constexpr const char * MCAST_IF = "127.0.0.1";

constexpr const char * IEX_GROUP = "239.77.7.1";
constexpr int IEX_PORT = 1234;

constexpr uint64_t PCAP_PACKET_COUNT = 1000000;

inline constexpr int BUCKETS = 4;
inline constexpr size_t RING_SIZE = 262144;
inline constexpr double LOSS_PERCENT = 0.05;

inline constexpr uint64_t SEED = 42;
//Repair TCP port: 
inline constexpr int RETRANS_PORT = 7799;
//Starting multicast port for the buckets (goes ...7.1, ...7.2, ...7.3, ...7.4)
inline constexpr const char *GROUP_BASE = "239.77.7.1";
// where multicaast UDP port starts for the buckets (goes 1234, 1235, 1236, 1237)
inline constexpr int DATA_PORT_BASE = 1234;
