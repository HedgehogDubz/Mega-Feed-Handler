// checkcap -- confirm a capture is present and actually usable, before
// spending twenty seconds discovering it is not.
//
// "The file exists" is not the same as "the pipeline can read it": the reader
// needs pcapng specifically (IEX names its downloads .pcap, but they are
// pcapng inside), and a download interrupted partway through will open fine
// and then run dry early. This reads the whole file the way exchange does and
// reports what is really there.
//
//   ./bin/checkcap                     # checks PCAP_FILE from src/env.h
//   ./bin/checkcap some/other.pcap     # checks a specific file

#include "env.h"
#include "pcapng_reader.h"
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : PCAP_FILE;
    std::printf("checkcap: %s\n", path);

    PcapngReader reader;
    if (!reader.open(path)) {
        
        std::fprintf(stderr, "checkcap: UNUSABLE\n");
        return 1;
    }

    std::vector<uint8_t> payload;
    uint64_t timestamp_ns = 0, packets = 0, bytes = 0, first_ns = 0, last_ns = 0;
    while (reader.next_packet(payload, timestamp_ns)) {
        if (!packets)
            first_ns = timestamp_ns;
        last_ns = timestamp_ns;
        packets++;
        bytes += payload.size();
    }

    std::printf("  UDP payloads readable: %llu\n", (unsigned long long)packets);
    std::printf("  payload bytes:         %llu\n", (unsigned long long)bytes);
    if (packets > 1) {
        std::printf("  capture spans:         %.1f s of market time\n",
                    (double)(last_ns - first_ns) / 1e9);
    }

    if (packets == 0) {
        std::fprintf(stderr, "checkcap: opens as pcapng but contains no UDP "
                             "payloads -- wrong feed, or truncated at the "
                             "very start\n");
        return 1;
    }
    if (packets < PCAP_PACKET_COUNT) {
        std::fprintf(stderr,
                     "checkcap: only %llu packets, but PCAP_PACKET_COUNT is "
                     "%llu -- the replay will simply stop early. Fetch more, "
                     "or lower PCAP_PACKET_COUNT in src/env.h\n",
                     (unsigned long long)packets,
                     (unsigned long long)PCAP_PACKET_COUNT);
        return 2; // usable, just shorter than configured
    }
    std::printf("checkcap: OK -- enough for PCAP_PACKET_COUNT=%llu\n",
                (unsigned long long)PCAP_PACKET_COUNT);
    return 0;
}
