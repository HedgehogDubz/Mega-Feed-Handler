#pragma once
#include "helpers.h"
#include <cstdint>
#include <string>
#include <unordered_map>
inline void copy_symbol(char (&destination)[9], const uint8_t *bytes) {
    int length = 8;
    while (length > 0 &&
           (bytes[length - 1] == ' ' || bytes[length - 1] == '\0')) {
        length--;
    }
    std::memcpy(destination, bytes, (size_t)length);
    destination[length] = '\0';
}

struct Level {
    std::unordered_map<int64_t, int64_t> side[2];//0 is bids, 1 is asks
};
inline std::unordered_map<std::string, struct Level> levels;

struct DeepPlusDecoder {

    struct Order {
        char symbol[9];
        int64_t price;
        uint32_t size;
        uint8_t side;
    };

    std::unordered_map<uint64_t, Order> orders;

    uint64_t unknown_references = 0;


    void addLevel(OrderMessage out[2], int &count, const Order &order,
                  int64_t delta, uint64_t timestamp) {
        auto &level_sizes = levels[order.symbol].side[order.side];
        
        int64_t new_total = (level_sizes[order.price] += delta);

        if (new_total <= 0) {
            level_sizes.erase(order.price);
            new_total = 0;
        }

        OrderMessage &update = out[count++];
        update = OrderMessage{};
        update.type = order.side ? 'S' : 'B';
        std::memcpy(update.symbol, order.symbol, sizeof(update.symbol));
        update.price = order.price;
        update.size = (uint32_t)new_total;
        update.timestamp = timestamp;

    }



    int decode(const uint8_t *bytes, size_t length, OrderMessage out[2]) {
        if (length < 26) {
            return 0;
        }

        uint64_t timestamp = get_u64(bytes + 2);

        int count = 0;

        constexpr unsigned char ORDER_ADD = 0x61;
        constexpr unsigned char SIDE_BUY = 0x38;
        constexpr unsigned char SIDE_SELL = 0x35;

        if (bytes[0] == ORDER_ADD && length >= 38 &&
            (bytes[1] == SIDE_BUY || bytes[1] == SIDE_SELL)) {
            Order order {};
            order.side = (bytes[1] == SIDE_SELL);
            copy_symbol(order.symbol, bytes + 10);
            
            order.size = get_u32(bytes + 26);
            order.price = (int64_t)get_u64(bytes + 30);

            const uint64_t orderID = get_u64(bytes + 18);
            orders[orderID] = order;

            //add its size to the level
            addLevel(out, count, order, (int64_t)order.size, timestamp);
            return count;
        }
        if (bytes[0] == 0x52 && length >= 26) { // 'R' order delete
            auto it = orders.find(get_u64(bytes + 18));
            if (it == orders.end()) {
                // Never saw the add -- we joined mid-stream, or bytes were
                // lost. There is no way to know its price or size, so nothing
                // can be emitted.
                unknown_references++;
                return 0;
            }
            // A delete REMOVES the order's full remaining size from its level.
            addLevel(out, count, it->second, -(int64_t)it->second.size, timestamp);
            // Stop tracking it -- the map must not accumulate dead orders.
            orders.erase(it);
            return count;
        }
        if (bytes[0] == 0x4d && length >= 38) { // 'M' order modify
            auto it = orders.find(get_u64(bytes + 18));
            if (it == orders.end()) {
                unknown_references++;
                return 0;
            }
            // A reference, so the updates at the bottom persist in the map.
            Order &order = it->second;
            uint32_t new_size = get_u32(bytes + 26);
            int64_t new_price = (int64_t)get_u64(bytes + 30);
            if (new_price == order.price) {
                // Same price: one level changed, by the difference in size
                // only.
                addLevel(out, count, order, (int64_t)new_size - (int64_t)order.size,
                     timestamp);
            } else {
                // Price moved: TWO levels changed, which is why out[] holds 2.
                // Remove the old size at the old price ...
                addLevel(out, count, order, -(int64_t)order.size,
                     timestamp); // level it leaves
                // ... update the price BEFORE the second addLevel, so it lands on
                // the new level ...
                order.price = new_price;
                // ... and add the new size there.
                addLevel(out, count, order, (int64_t)new_size,
                     timestamp); // level it joins
            }
            // Record the new size for whatever event comes next.
            order.size = new_size;
            return count;
        }

        if (bytes[0] == 0x4c && length >= 46) { // 'L' order executed
            auto it = orders.find(get_u64(bytes + 18));
            if (it == orders.end()) {
                unknown_references++;
                return 0;
            }
            Order &order = it->second;
            // Offsets 26..29: how many shares were filled (not the order's full
            // size).
            uint32_t executed = get_u32(bytes + 26);
            // The filled shares leave the resting level.
            addLevel(out, count, order, -(int64_t)executed, timestamp);

            OrderMessage &trade = out[count++]; // an execution IS a trade
            trade = OrderMessage{};
            trade.type = 'T';
            // The order remembers the symbol; the message body does not repeat
            // it here.
            std::memcpy(trade.symbol, order.symbol, sizeof trade.symbol);
            trade.price =
                (int64_t)get_u64(bytes + 30); // trade price from the message
            trade.size = executed;
            trade.timestamp = timestamp;
            // A fully-filled order is gone from the book with no 'R' to follow,
            // so it must be dropped here or it would leak and corrupt later
            // levels.
            if (executed >= order.size)
                orders.erase(it); // fully filled: no 'R' follows
            else
                order.size -= executed;
            return count;
        }

        if (bytes[0] == 0x54 && length >= 38) { // 'T' trade report
            OrderMessage &trade = out[count++];          // (non-displayed matches)
            trade = OrderMessage{};
            trade.type = 'T';
            // A standalone trade carries its own symbol -- there is no resting
            // order to look it up from, which is the point: this is a hidden
            // match.
            copy_symbol(trade.symbol, bytes + 10);
            // Note the offsets differ from 'L': no order id field in this
            // layout.
            trade.size = get_u32(bytes + 18);
            trade.price = (int64_t)get_u64(bytes + 22);
            trade.timestamp = timestamp;
            // No addLevel: a non-displayed match never rested on the visible book,
            // so no level changes.
            return count;
        }
        return 0;
    }
};
