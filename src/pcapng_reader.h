#pragma once
#include "helpers.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct PcapngReader {

    FILE *file = nullptr;

    bool isSwappedEndian = false;
    bool warned_truncated = false;
    bool warned_link_type = false;

    // A corrupt or desynced block length must not become a multi-GB
    // allocation; no real pcapng block comes near this.
    static constexpr uint32_t MAX_BLOCK_LENGTH = 16u << 20;

    struct InterfaceInfo {
        uint32_t link_type;
        uint64_t ns_multiplier, ns_divisor;
    };
    std::vector<InterfaceInfo> interfaces;

    PcapngReader() = default;
    ~PcapngReader() { close_file(); }
    PcapngReader(const PcapngReader &) = delete;
    PcapngReader &operator=(const PcapngReader &) = delete;

    void close_file() {
        if (file) {
            fclose(file);
            file = nullptr;
        }
    }

    bool open(const std::string &filename) {
        close_file(); // reopening must not leak the previous handle
        file = fopen(filename.c_str(), "rb");
        if (!file) {
            perror("fopen");
            return false;
        }
        uint8_t magic_bytes[4];
        if (fread(magic_bytes, 1, 4, file) != 4) {
            fprintf(stderr, "%s: too short to be a capture file\n",
                    filename.c_str());
            close_file();
            return false;
        }
        uint32_t magic = get_u32(magic_bytes);
        if (magic != 0x0A0D0D0A) {
            if (magic == 0xA1B2C3D4 || magic == 0xD4C3B2A1 ||
                magic == 0xA1B23C4D || magic == 0x4D3CB2A1) {
                fprintf(stderr,
                        "%s: classic pcap is not supported, only pcapng\n",
                        filename.c_str());
            } else {
                fprintf(stderr, "%s: not a pcapng file (magic 0x%08x)\n",
                        filename.c_str(), magic);
            }
            close_file();
            return false;
        }
        fseek(file, 0, SEEK_SET);
        return true;
    }

    uint16_t order_u16(const uint8_t *bytes) {
        uint16_t value = get_u16(bytes);
        if (isSwappedEndian) {
            value = (uint16_t)((value >> 8) | (value << 8));
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

    // Protocol fields on the wire are big-endian no matter what byte order the
    // capture file itself uses, so they must not go through order_u16.
    static uint16_t net_u16(const uint8_t *bytes) {
        return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
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
                                      uint32_t &captured_length,
                                      uint64_t &timestamp_ns,
                                      uint32_t &wire_length,
                                      uint32_t &packet_link_type) {
        if (!file) {
            return false;
        }
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
                if (block_total_length < 28 ||
                    block_total_length > MAX_BLOCK_LENGTH) {
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
            if (total_length < 12 || total_length > MAX_BLOCK_LENGTH) {
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
                    // if_tsresol: bit 7 selects the base. Set means the
                    // remaining bits are a negative power of two, clear means a
                    // negative power of ten.
                    if (option_code == 9 && option_length >= 1 &&
                        option_offset + 4 < option_end) {
                        uint8_t timestamp_resolution = body[option_offset + 4];
                        uint8_t exponent = timestamp_resolution & 0x7f;
                        if (timestamp_resolution & 0x80) {
                            if (exponent < 64) {
                                interface_info.ns_multiplier = 1000000000ull;
                                interface_info.ns_divisor = 1ull << exponent;
                            }
                        } else if (exponent <= 9) {
                            interface_info.ns_multiplier = pow10(9 - exponent);
                            interface_info.ns_divisor = 1;
                        }
                    }
                    option_offset += 4 + ((option_length + 3) & ~3);
                }
                interfaces.push_back(interface_info);
                continue;
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
                // 20 bytes of EPB header plus the 4-byte trailing length.
                if ((size_t)captured_length + 24 > remaining_length) {
                    continue;
                }
                const InterfaceInfo &interface_info =
                    (interface_id < interfaces.size()
                         ? interfaces[interface_id]
                         : InterfaceInfo(1, 1000ull, 1ull));

                packet_link_type = interface_info.link_type;
                // Widened: with a power-of-two resolution the multiply alone
                // overflows 64 bits before the divide brings it back.
                timestamp_ns = (uint64_t)((unsigned __int128)ticks *
                                          interface_info.ns_multiplier /
                                          interface_info.ns_divisor);
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
            uint32_t captured_length = 0, wire_length = 0, packet_link_type = 0;
            if (!read_until_next_packet_block(packet_data, captured_length,
                                              timestamp_ns, wire_length,
                                              packet_link_type)) {
                return false;
            }
            if (captured_length < wire_length) {
                if (!warned_truncated) {
                    warned_truncated = true;
                    fprintf(stderr,
                            "Warning: truncated packet (%u of %u bytes); "
                            "skipping truncated packets\n",
                            captured_length, wire_length);
                }
                continue;
            }
            if (packet_link_type != 1) {
                if (!warned_link_type) {
                    warned_link_type = true;
                    fprintf(stderr,
                            "Warning: link type %u is not ethernet; skipping "
                            "those packets\n",
                            packet_link_type);
                }
                continue;
            }
            // ethernet only
            if (captured_length < 14) {
                continue;
            }
            if (net_u16(packet_data.data() + 12) != 0x0800) { // IPv4 only
                continue;
            }
            constexpr size_t offset = 14;
            if (captured_length < offset + 20) {
                continue;
            }
            const uint8_t *ip_header = packet_data.data() + offset;
            if ((ip_header[0] >> 4) != 4) {
                continue;
            }
            size_t ip_header_length = (size_t)(ip_header[0] & 0x0F) * 4;
            // room for the IP header and the 8-byte UDP header we skip below
            if (ip_header_length < 20 ||
                captured_length < offset + ip_header_length + 8) {
                continue;
            }
            if (ip_header[9] != 17) { // UDP only
                continue;
            }
            const uint8_t *udp = ip_header + ip_header_length;
            size_t udp_total = net_u16(udp + 4);
            if (udp_total < 8) {
                continue;
            }
            // Length from the UDP header, not the end of the frame: ethernet
            // pads short frames to 60 bytes and that padding is not payload.
            size_t payload_offset = offset + ip_header_length + 8;
            size_t payload_length = udp_total - 8;
            if (payload_offset + payload_length > captured_length) {
                payload_length = captured_length - payload_offset;
            }
            payload.assign(
                packet_data.begin() + (long)payload_offset,
                packet_data.begin() + (long)(payload_offset + payload_length));
            return true;
        }
    }
};
