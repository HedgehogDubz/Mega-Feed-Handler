#pragma once
#include "helpers.h"
#include <algorithm>

struct IextpHeader {
    uint8_t version;
    uint16_t protocol, payload_length, message_count;
    uint32_t channel, session;
    uint64_t stream_offset, first_sequence_number, send_ns;
};

inline bool iextp_decode(const uint8_t *packet, size_t length, IextpHeader &header) {
    if (length < 40) {
        return false;
    }
    header.version = packet[0];
    header.protocol = get_u16(packet + 2); //0x8005 == DEEP+ protocol //0x8004 class DEEPXXXX
    header.channel = get_u32(packet + 4);
    header.session = get_u32(packet + 8);
    header.payload_length = get_u16(packet + 12);
    header.message_count = get_u16(packet + 14);
    header.stream_offset = get_u64(packet + 16);
    header.first_sequence_number = get_u64(packet + 24);
    header.send_ns = get_u64(packet + 32);

    return header.version == 1;
}

template <typename F>
inline void iextp_for_each_message(const uint8_t *packet, size_t length, const IextpHeader &header, F callback) {
    size_t offset = 40;
    size_t end = std::min(length, (size_t)header.payload_length + 40);

    for(uint32_t i = 0; i < header.message_count && offset + 2 <= end; ++i) {
        uint16_t message_length = get_u16(packet + offset);
        offset += 2;
        if (offset + message_length > end) {
            break;
        }
        callback(packet + offset, (size_t)message_length);
        offset += message_length;
    }
}
