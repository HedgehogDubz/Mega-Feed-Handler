#pragma once

#include <cstdint>
#include <cstring>

// turn to little-endian form
inline void put_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}
inline void put_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}
inline void put_u64(uint8_t *bytes, uint64_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
    bytes[4] = (uint8_t)(value >> 32);
    bytes[5] = (uint8_t)(value >> 40);
    bytes[6] = (uint8_t)(value >> 48);
    bytes[7] = (uint8_t)(value >> 56);
}
inline uint16_t get_u16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}
inline uint32_t get_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}
inline uint64_t get_u64(const uint8_t *bytes) {
    return (uint64_t)bytes[0] | ((uint64_t)bytes[1] << 8) |
           ((uint64_t)bytes[2] << 16) | ((uint64_t)bytes[3] << 24) |
           ((uint64_t)bytes[4] << 32) | ((uint64_t)bytes[5] << 40) |
           ((uint64_t)bytes[6] << 48) | ((uint64_t)bytes[7] << 56);
}

// based on IEX format price * 10000
//  Signed: IEX encodes price as a signed 8-byte fixed-point value, so an
//  unsigned parameter would turn any negative price into ~1.8e15.
constexpr int64_t PRICE_MULTIPLIER = 10000;
inline double price_to_double(int64_t price) {
    return (double)price / (double)PRICE_MULTIPLIER;
}

struct OrderMessage {
    char type = 0;
    char symbol[9];
    int64_t price = 0;
    uint32_t size = 0;
    uint64_t timestamp = 0;
};
constexpr size_t ORDER_MESSAGE_SIZE = sizeof(OrderMessage);
constexpr size_t HDR_BYTES = 16;
constexpr uint32_t MAGIC = 0x4d464844; // "MFHD"
constexpr size_t MAX_REPAIR_BATCH = 512;
inline size_t dgram_encode(uint8_t *bytes, uint16_t bucket, uint64_t sequence,
                           const uint8_t *msg_bytes) {
    put_u32(bytes, MAGIC);
    put_u16(bytes + 4, bucket);
    put_u64(bytes + 6, sequence);
    put_u16(bytes + 14, (uint16_t)ORDER_MESSAGE_SIZE);
    memcpy(bytes + HDR_BYTES, msg_bytes, ORDER_MESSAGE_SIZE);
    return HDR_BYTES + ORDER_MESSAGE_SIZE;
}

struct Dgram {
    uint16_t bucket;
    uint64_t sequence;
    OrderMessage message;
};
OrderMessage OrderMessage_decode(const uint8_t *bytes);
inline bool dgram_decode(const uint8_t *bytes, size_t length, Dgram &out) {
    if (length < HDR_BYTES + ORDER_MESSAGE_SIZE || get_u32(bytes) != MAGIC)
        return false;
    out.bucket = get_u16(bytes + 4);
    out.sequence = get_u64(bytes + 6);
    if (get_u16(bytes + 14) != ORDER_MESSAGE_SIZE)
        return false;
    out.message = OrderMessage_decode(bytes + HDR_BYTES);
    return true;
}
inline uint64_t fnv1a(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < length; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
inline void OrderMessage_encode(uint8_t *bytes,
                                const OrderMessage &OrderMessage) {
    bytes[0] = (uint8_t)OrderMessage.type;
    std::memcpy(bytes + 1, OrderMessage.symbol, 8);
    put_u64(bytes + 9, (uint64_t)OrderMessage.price);
    put_u32(bytes + 17, OrderMessage.size);
    put_u64(bytes + 21, OrderMessage.timestamp);
}
inline OrderMessage OrderMessage_decode(const uint8_t *bytes) {
    OrderMessage om;
    om.type = (char)bytes[0];
    memcpy(om.symbol, bytes + 1, 8);
    om.symbol[8] = 0;
    om.price = (int64_t)get_u64(bytes + 9);
    om.size = get_u32(bytes + 17);
    om.timestamp = get_u64(bytes + 21);
    return om;
}
inline int bucket_of(const char *symbol, int bucket_count) {
    return (int)(fnv1a(symbol, strlen(symbol)) % (uint64_t)bucket_count);
}
struct Fingerprint {
    uint64_t hash = 6942098103934665603ull;
    void mix(uint64_t value) {
        for (int i = 0; i < 8; i++) {
            hash ^= (value >> 8 * i) & 0xff;
            hash *= 1099511628211ull;
        }
    }
};