// visualizer -- draws one symbol's order book as a live ladder in the
// terminal, about ten frames a second.
//
// It joins ONLY the multicast bucket that carries its symbol: the point of
// bucketing is that a consumer's machine never even receives the traffic it
// doesn't want. The book itself can stay tiny because the feed guarantees
// every message arrives exactly once, in order -- all the hard work already
// happened upstream.

#include "book.h"
#include "env.h"
#include "subscriber_env.h"
#include "net.h"
#include "subscriber.h"
#include "helpers.h"
#include <string>
#include <vector>

static std::string bar(uint32_t size) {
  return std::string(std::min<uint32_t>(size / 150, 40), '#');
}

static void draw(const std::string &symbol, const Book &book,
                 const OrderMessage &last_trade, const SubscriberStats &stats) {
  printf("\033[H\033[2J"); // cursor home + clear screen
  printf("  %s\n", symbol.c_str());
  printf("  feed: %llu delivered  %llu repaired  %llu unrecoverable  %llu "
         "repair requests\n\n",
         (unsigned long long)stats.delivered,
         (unsigned long long)stats.repaired,
         (unsigned long long)stats.unrecoverable,
         (unsigned long long)stats.repair_requests);

  // Asks print worst-first so the two best prices meet in the middle.
  std::vector<std::pair<int64_t, uint32_t>> ask_levels;
  for (auto it = book.asks.begin();
       it != book.asks.end() && ask_levels.size() < 8; ++it)
    ask_levels.push_back(*it);
  for (auto it = ask_levels.rbegin(); it != ask_levels.rend(); ++it)
    printf("            %10.2f  %-7u %s\n", price_to_double(it->first), it->second,
           bar(it->second).c_str());

  Bbo quote = book.bbo();
  if (quote.bid_price && quote.ask_price)
    printf("   ------------ spread %.2f ------------\n",
           price_to_double(quote.ask_price - quote.bid_price));
  else
    printf("   ------------ (one-sided) ------------\n");

  int shown = 0;
  for (auto it = book.bids.begin(); it != book.bids.end() && shown < 8;
       ++it, ++shown)
    printf("            %10.2f  %-7u %s\n", price_to_double(it->first), it->second,
           bar(it->second).c_str());

  if (last_trade.type == 'T')
    printf("\n  last trade: %u @ %.2f\n", last_trade.size,
           price_to_double(last_trade.price));
  fflush(stdout);
}

int main() {
  install_sigint();
  std::string symbol = WATCH_SYMBOL;
  int bucket_count = BUCKETS;
  int bucket_id = bucket_of(symbol.c_str(), bucket_count);
  printf("visualizer: %s lives in bucket %d of %d -- joining only that group\n",
         symbol.c_str(), bucket_id, bucket_count);
  printf("visualizer: waiting for the feed...\n");
  fflush(stdout); // stdout to a file is block-buffered: without this, a
                  // visualizer that is merely waiting appears to print nothing

  // Join live (0), rather than demanding history from sequence 1. A ladder is
  // a picture of the market NOW: replaying a quarter of a million historical
  // messages over TCP repair only to overdraw them within milliseconds costs
  // the repair server real bandwidth and buys the viewer nothing.
  //
  // The trade-off is an incomplete book at first: a price level set before we
  // tuned in, and not touched since, is not in our map. It fills in as levels
  // update. Pass 1 instead if you need the whole session reconstructed.
  Subscriber sub({bucket_id}, 0, BACKFILL_ON_JOIN);
  Book book;
  OrderMessage last_trade;
  uint64_t last_draw = 0;

  bool ended = sub.run([&](int, const OrderMessage &msg) {
    if (symbol != msg.symbol)
      return; // other symbols share the bucket; not our problem
    if (!book.apply(msg))
      fprintf(stderr,
              "visualizer: crossed book on %s -- upstream data is wrong\n",
              msg.symbol);
    if (msg.type == 'T')
      last_trade = msg;
    uint64_t now = now_ns();
    if (now - last_draw >
        100000000ull) { // ~10 fps; drawing every message would
      last_draw = now;  // burn the core the feed needs
      draw(symbol, book, last_trade, sub.stats());
    }
  });

  (void)ended;
  draw(symbol, book, last_trade, sub.stats());
  SubscriberStats stats = sub.stats();
  printf("\nstream ended. delivered=%llu repaired=%llu unrecoverable=%llu "
         "duplicates-ignored=%llu\n",
         (unsigned long long)stats.delivered,
         (unsigned long long)stats.repaired,
         (unsigned long long)stats.unrecoverable,
         (unsigned long long)stats.duplicates);
  return 0;
}
