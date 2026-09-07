#pragma once
// <cstdint> explicitly for uint64_t below: libc++ happens to pull it in via
// <string>, libstdc++ does not, so relying on that builds on macOS and fails
// on Linux.
#include <cstdint>
#include <string>

// inline: without it every translation unit gets its own copy and any TU that
// does not use it warns. Note this only stays constexpr while the value is
// short enough for the small-string buffer -- a longer symbol would need to
// allocate, which a constexpr object at namespace scope cannot do.
inline constexpr std::string WATCH_SYMBOL = "MSFT";
// Joining live leaves the book missing any price level that was set before we
// arrived and has not been touched since. This asks for that many recent
// sequences over TCP repair to fill it in -- and is DELIBERATELY 0, because
// measured on a late join it does far more harm than good:
//
//   20000 -> 115.85 s CPU, 133094 unrecoverable, 67544 delivered
//       0 ->   0.25 s CPU,      0 unrecoverable, 180446 delivered
//
// The reason is that repair works one sequence at a time. Asking for 20000
// messages creates 20000 holes at once; servicing them starves the receive
// loop, the UDP buffer overflows, that manufactures MORE holes, and the whole
// thing spirals until the give-up deadline mass-writes the backlog off.
//
// Filling a book is a job for a SNAPSHOT -- a few hundred bytes of price
// levels -- not for replaying tens of thousands of individual messages. Raise
// this only with small values, and measure.
inline constexpr uint64_t BACKFILL_ON_JOIN = 0;
