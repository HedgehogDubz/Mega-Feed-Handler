#include "helpers.h"
#include <cstdint>
#include <cstdio>
#include <print>
#include <vector>

struct PcapngReader {

    FILE *file = nullptr;

    bool isSwappedEndian = false;
    bool nanosecond_timestamps = false;
    bool warned_truncated = false;

    struct InterfaceInfo {
        uint32_t link_type;
        uint64_t ns_multiplier, ns_divisor;
    };
    std::vector<InterfaceInfo> interfaces;

    bool open(const std::string &filename) {
        file = fopen(filename.c_str(), "rb");
        if (!file) {
            perror("fopen");
            return false;
        }
        uint8_t global_header[24];
        if (fread(global_header, 1, 4, file) != 4) {
            perror("capture to short");
            return false;
        }
        uint32_t magic = get_u32(global_header);
        if (magic != 0x0A0D0D0A) {
            fseek(file, 0, SEEK_SET);
            return true;
        }
        if (fread(global_header + 4, 1, 20, file) != 20) {
            perror("capture to short");
            return false;
        }
        fseek(file, 0, SEEK_SET);
        return true;
    }

    uint16_t order_u16(const uint8_t *bytes) {
        uint16_t value = get_u16(bytes);
        if (isSwappedEndian) {
            value = (value >> 8) | (value << 8);
        }
        return value;
    }
    uint32_t order_u32(const uint8_t *bytes) {
        uint32_t value = get_u32(bytes);
        if (isSwappedEndian) {
            value = ((value >> 24) & 0x000000FF) | ((value >> 8) & 0x0000FF00) |
                    ((value << 8) & 0x00FF0000) | ((value << 24) & 0xFF000000);
        }
        return value;
    }

    unsigned long long pow10(long long n) {
        unsigned long long result = 1;
        for (long long i = 0; i < n; ++i) {
            result *= 10;
        }
        return result;
    }

    // read the blocks until the next packet block is found, return the
    // interface id of the packet block, or -1 if EOF
    bool read_until_next_packet_block(std::vector<uint8_t> &packet_data,
                                      uint32_t captured_length,
                                      uint64_t &timestamp_ns,
                                      uint32_t &wire_length,
                                      uint32_t &packet_link_type) {
        std::vector<uint8_t> body;

        for (;;) {
            uint8_t block_header[8];
            if (fread(block_header, 1, 8, file) != 8) {
                return false;
            }
            if (get_u32(block_header) == 0x0a0d0d0a) {
                uint8_t byte_order_magic[4];
                if (fread(byte_order_magic, 1, 4, file) != 4) {
                    return false;
                }
                // update if isSwappedEndian_ based on byte_order_magic
                if (get_u32(byte_order_magic) == 0x1a2b3c4d) {
                    isSwappedEndian = false;
                } else if (get_u32(byte_order_magic) == 0x4d3c2b1a) {
                    isSwappedEndian = true;
                } else {
                    fprintf(stderr, "Unknown byte order magic: 0x%08x\n",
                            get_u32(byte_order_magic));
                    return false;
                }
                uint32_t block_total_length = order_u32(block_header + 4);
                if (block_total_length < 28) {
                    fprintf(stderr, "Invalid block total length: %u\n",
                            block_total_length);
                    return false;
                }
                fseek(file, block_total_length - 12, SEEK_CUR);
                interfaces.clear();
                continue;
            }
            uint32_t block_type = order_u32(block_header);
            uint32_t total_length = order_u32(block_header + 4);
            if (total_length < 12) {
                fprintf(stderr, "Invalid block total length: %u\n",
                        total_length);
                return false;
            }
            uint32_t remaining_length = total_length - 8;

            if (block_type == 1) {
                body.resize(remaining_length);
                if (fread(body.data(), 1, remaining_length, file) !=
                    remaining_length) {
                    return false;
                }
                InterfaceInfo interface_info(order_u16(body.data()), 1000ull,
                                             1ull);

                size_t option_offset = 8;
                size_t option_end =
                    (remaining_length >= 4) ? remaining_length - 4 : 0;
                while (option_offset + 4 <= option_end) {
                    uint16_t option_code =
                        order_u16(body.data() + option_offset);
                    uint16_t option_length =
                        order_u16(body.data() + option_offset + 2);
                    if (option_code == 0) {
                        break;
                    }
                    if (option_code == 9 && option_length >= 1 &&
                        option_offset + 4 < option_end) {
                        uint8_t timestamp_resolution = body[option_offset + 4];
                        if (timestamp_resolution == 9) {
                            interface_info.ns_multiplier = 1000000000ull;
                            interface_info.ns_divisor =
                                1ull << (timestamp_resolution & 0x3f);
                        } else if (timestamp_resolution <= 9 &&
                                   option_length >= 1 &&
                                   option_offset + 4 < option_end) {
                            interface_info.ns_multiplier =
                                pow10(9 - timestamp_resolution);
                            interface_info.ns_divisor = 1;
                        } else if (timestamp_resolution == 3) {
                            interface_info.ns_multiplier = 1;
                            interface_info.ns_divisor =
                                pow10(9 - timestamp_resolution);
                        }
                    }
                    option_offset += 4 + ((option_length + 3) & ~3);
                }
                interfaces.push_back(interface_info);
            }
            if (block_type == 6) {
                body.resize(remaining_length);
                if (fread(body.data(), 1, remaining_length, file) !=
                    remaining_length) {
                    return false;
                }
                uint32_t interface_id = order_u32(body.data());
                uint64_t ticks = (uint64_t)order_u32(body.data() + 4) << 32 |
                                 ((uint64_t)order_u32(body.data() + 8));

                captured_length = order_u32(body.data() + 12);
                wire_length = order_u32(body.data() + 16);
                if ((size_t)captured_length + 20 > remaining_length) {
                    continue;
                }
                const InterfaceInfo &interface_info =
                    (interface_id < interfaces.size()
                         ? interfaces[interface_id]
                         : InterfaceInfo(1, 1000ull, 1ull));

                packet_link_type = interface_info.link_type;
                timestamp_ns = ticks * interface_info.ns_multiplier /
                               interface_info.ns_divisor;
                packet_data.resize(captured_length);
                packet_data.assign(body.begin() + 20,
                                   body.begin() + 20 + captured_length);
                return true;
            }
            fseek(file, remaining_length, SEEK_CUR);
        }
    }
    // hand back the UDP payload with its capture timestamp
    bool next_packet(std::vector<uint8_t> &payload, uint64_t &timestamp_ns) {
        std::vector<uint8_t> packet_data;
        for (;;) {
            uint32_t captured_length, wire_length, packet_link_type;
            if (!read_until_next_packet_block(packet_data, captured_length,
                                              timestamp_ns, wire_length,
                                              packet_link_type)) {
                return false;
            }
            if (captured_length < wire_length && !warned_truncated) {
                std::perror("Warning: packet truncated");
                continue;
            }
            if (packet_link_type != 1) {
                std::perror(
                    "Unsupported link type, only ethernet is supported");
                return false;
            }
            // ethernet only
            if (captured_length < 14) {
                std::perror("Packet too short for ethernet header");
                continue;
            }
            uint16_t ethertype = order_u16(packet_data.data() + 12);
            if (ethertype != 0x0800) {
                std::perror("Unsupported ethertype, only IPv4 is supported");
                continue;
                constexpr size_t offset = 14;
                if (captured_length < offset + 20) {
                    std::perror("Packet too short for IPv4 header");
                    continue;
                }
                const uint8_t *ip_header = packet_data.data() + offset;
                if ((ip_header[0] >> 4) != 4) {
                    std::perror(
                        "Unsupported IP version, only IPv4 is supported");
                    continue;
                }
                uint8_t ip_header_length = (ip_header[0] & 0x0F) * 4;
                if (captured_length < offset + ip_header_length) {
                    std::perror("Packet too short for IPv4 header length");
                    continue;
                }
                payload.assign(packet_data.begin() +
                                   (long)(offset + ip_header_length + 8),
                               packet_data.end());
                return true;
            }
        }
    }
};