#pragma once
// book.h -- a running picture of the market, and the checks that keep it
// honest.

#include "helpers.h"
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>

struct Bbo { // best bid and offer: the two numbers that actually matter
  // The highest price anyone is currently willing to BUY at, and how much.
  int64_t bid_price = 0;
  uint32_t bid_size = 0;
  // The lowest price anyone is currently willing to SELL at, and how much.
  int64_t ask_price = 0;
  uint32_t ask_size = 0;

  // Field-by-field inequality. Used to answer "did the touch actually move?"
  // so the fingerprint is only stirred on real changes, not on every deep
  // update that left the best prices untouched.
  bool operator!=(const Bbo &other) const {
    return bid_price != other.bid_price || bid_size != other.bid_size ||
           ask_price != other.ask_price || ask_size != other.ask_size;
  }
};

struct Book {
  // Bids sorted high-to-low, asks low-to-high: begin() is always the touch.
  // std::greater flips the bid comparator so the BEST bid (highest) sorts
  // first; asks use the default ascending order so the best ask (lowest) is
  // also first. One rule -- "begin() is best" -- for both sides.
  std::map<int64_t, uint32_t, std::greater<int64_t>> bids;
  std::map<int64_t, uint32_t> asks;

  // Returns false if the update produced an impossible book. Nobody sells
  // for less than someone else is willing to pay -- a crossed book means
  // OUR book is wrong. Checking every update makes it fail loudly at the
  // exact message that broke things, instead of producing quietly wrong
  // output for the next hour.
  bool apply(const OrderMessage &OrderMessage) {
    // "size zero" means DELETE this price level, not "a level with
    // nothing in it". Store the zero and the book slowly fills with
    // ghost prices that get reported as the best in the market.
    if (OrderMessage.type == 'B') {
      // Buy side: erase the level outright, or overwrite it with the new total.
      if (OrderMessage.size == 0)
        bids.erase(OrderMessage.price);
      else
        bids[OrderMessage.price] = OrderMessage.size;
    } else if (OrderMessage.type == 'S') {
      // Sell side: same two cases.
      if (OrderMessage.size == 0)
        asks.erase(OrderMessage.price);
      else
        asks[OrderMessage.price] = OrderMessage.size;
    } else
      return true; // trades don't change the standing book

    // The sanity check. An empty side can't cross, so those two cases are
    // fine by definition; otherwise the best bid must be strictly BELOW the
    // best ask. begin() on each map is the touch, per the comparators above.
    return bids.empty() || asks.empty() ||
           bids.begin()->first < asks.begin()->first;
  }

  // Snapshot the top of each side.
  Bbo bbo() const {
    // Starts zeroed, so an empty side reads back as 0 rather than as garbage.
    Bbo quote;
    if (!bids.empty()) {
      // begin() is the highest bid: ->first is the price key, ->second the size.
      quote.bid_price = bids.begin()->first;
      quote.bid_size = bids.begin()->second;
    }
    if (!asks.empty()) {
      // begin() is the lowest ask.
      quote.ask_price = asks.begin()->first;
      quote.ask_size = asks.begin()->second;
    }
    return quote;
  }
};

// Every symbol of one bucket, plus the fingerprint of every BBO change.
// The publisher runs one of these over the messages BEFORE loss is injected
// (correct by construction); each consumer runs one over what actually
// arrived. Equal fingerprints = every message, exactly once, in order.
struct BookSet {
  // One Book per symbol name. unordered_map because lookup happens on every
  // single message and the ordering a std::map would buy us is never used.
  std::unordered_map<std::string, Book> books;
  // The previous touch per symbol, so a change can be detected rather than
  // re-mixed on every message.
  std::unordered_map<std::string, Bbo> last_quote;
  // The running order-sensitive hash of every touch change (see wire.h).
  Fingerprint fingerprint;
  // How many times an update produced an impossible book. Reported, not fatal.
  uint64_t crossed = 0;
  // Printing every one of these is self-defeating: on a bad feed it is
  // hundreds of thousands of formatted stderr writes inside the publish hot
  // path, which starves the receive loop and causes more loss than it
  // diagnoses. Name the first few, then let `crossed` do the counting.
  static constexpr uint64_t CROSSED_PRINT_MAX = 10;

  void apply(const OrderMessage &OrderMessage) {
    // The end-of-stream marker is bookkeeping, not market data -- mixing it
    // in would put a message into the fingerprint that isn't a price change.
    if (OrderMessage.type == 'E')
      return;

    // operator[] default-constructs an empty Book the first time a symbol
    // is seen, so there is no "is this symbol known yet" branch anywhere.
    Book &book = books[OrderMessage.symbol];
    if (!book.apply(OrderMessage)) {
      // Crossed. Count it and name the exact message, then keep going: a
      // consumer that dies here would lose the diagnostics that follow.
      crossed++;
      if (crossed <= CROSSED_PRINT_MAX) {
        fprintf(stderr,
                "IMPOSSIBLE BOOK: %s bid %.2f >= ask %.2f -- the book is "
                "wrong, and this is the exact message that broke it\n",
                OrderMessage.symbol, price_to_double(book.bids.begin()->first),
                price_to_double(book.asks.begin()->first));
        if (crossed == CROSSED_PRINT_MAX)
          fprintf(stderr, "IMPOSSIBLE BOOK: further reports suppressed; the "
                          "running total is reported at exit\n");
      }
    }

    // Where the touch sits now, versus where it sat before this message.
    Bbo current = book.bbo();
    // A REFERENCE, so the assignment below updates the stored value in place.
    Bbo &previous = last_quote[OrderMessage.symbol];
    if (current != previous) {
      // Remember the new touch for the next comparison.
      previous = current;
      // Mix the symbol first, so the same price change on two different
      // symbols produces different contributions.
      fingerprint.mix(fnv1a(OrderMessage.symbol, strlen(OrderMessage.symbol)));
      // Then all four touch numbers, always in this same order -- both the
      // publisher and every consumer must mix identically to ever agree.
      fingerprint.mix((uint64_t)current.bid_price);
      fingerprint.mix(current.bid_size);
      fingerprint.mix((uint64_t)current.ask_price);
      fingerprint.mix(current.ask_size);
    }
  }
};
