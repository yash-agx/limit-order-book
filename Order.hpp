#pragma once
#include <cstddef>
#include <cstdint>

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };

using OrderId  = std::uint64_t;
using Price    = std::int64_t;   // stored in cents: $101.20 -> 10120 (no floating-point errors)
using Quantity = std::uint32_t;
using Timestamp = std::int64_t;  // nanoseconds on std::chrono::steady_clock

struct Order {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;          // ignored for market orders
    Quantity  quantity;       // remaining (unfilled) quantity
    Timestamp timestamp = 0;  // when the book received it; set by OrderBook::addOrder
};

// Why the book refused an order (None = the order is fine).
enum class RejectReason { None, ZeroQuantity, DuplicateId };

// One row of market depth: everything resting at a single price, added together.
struct DepthLevel {
    Price       price;
    Quantity    quantity;    // total across all orders at this price
    std::size_t orderCount;
};

struct Trade {
    OrderId   buyId;
    OrderId   sellId;
    Price     price;          // always the resting order's price
    Quantity  quantity;
    Timestamp timestamp = 0;  // when it executed = arrival time of the incoming order that caused it
};
