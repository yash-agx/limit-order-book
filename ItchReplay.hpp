#pragma once
#include "OrderFlow.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Reads the action files written by analysis/itch_extract.py, which turns a day of Nasdaq
// TotalView-ITCH 5.0 messages for one symbol into the same Action type the synthetic order
// flow uses. The engines therefore replay real market data through exactly the same code
// path as the synthetic benchmark - apply(book, action) - and nothing in the engines changes.
//
// IMPORTANT, and said again wherever these numbers appear: ITCH is a feed of book events
// Nasdaq has already matched. Adds in it never cross the book. Replaying it measures the
// engines on real order-book *update* flow, not on real matching.
//
// File layout (little-endian, as written by the Python extractor):
//   a 128-byte header, then `prefill` records, then `actions` records, 24 bytes each.
// The prefill records are the book as it stood at 09:30:00 and are applied untimed;
// the records after them are the measured flow.

struct ItchFile {
    std::string symbol, date;

    std::vector<Action> prefill;   // the 09:30 book, applied untimed to set the book up
    std::vector<Action> flow;      // the measured actions (09:30 to 16:00)

    std::uint64_t messages = 0;    // ITCH messages behind `flow` (one per action)
    std::uint64_t peakLive = 0;    // most orders resting at once during the window
    Price         minPrice = 0, maxPrice = 0;
    std::uint64_t adds = 0, cancels = 0, modifies = 0;
    std::int64_t  firstNs = 0, lastNs = 0;   // nanoseconds since midnight

    // How many price levels the whole window touched. Engine B's flat level array has to
    // span this, so it is worth reporting.
    std::size_t priceSpan() const {
        return static_cast<std::size_t>(maxPrice - minPrice) + 1;
    }
};

// Read one action file. `maxActions` > 0 keeps only that many measured actions (the prefill
// is always read in full, or the book would be wrong). Throws std::runtime_error if the file
// is missing, truncated or not an action file.
ItchFile readItchFile(const std::string& path, std::size_t maxActions = 0);
