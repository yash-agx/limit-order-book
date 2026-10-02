#pragma once
#include "OrderBookBase.hpp"
#include <cstdint>
#include <deque>
#include <unordered_map>

// Implementation B - still only standard containers, but chosen to suit the data:
//
//   Price levels: a flat std::vector indexed by price. levels[price - basePrice_] is the
//                 level for that price - one subtraction instead of a walk down a tree.
//                 Works because prices are whole cents and the book spans few of them.
//   Queue:        each level is a std::vector<Order>, so the orders at one price sit next
//                 to each other in memory. A cancelled order is not removed straight away
//                 (that would shift everything after it); its slot is marked dead
//                 (quantity 0) and cleaned up later in batches.
//   Index:        order id -> location is an array indexed by the id itself (a std::deque,
//                 so it can grow without copying). No hashing. Ids too big for the array
//                 fall back to a std::unordered_map.
//
// The price to pay: memory grows with the price range and with the largest id ever seen,
// not just with the number of resting orders.
class FlatBook : public OrderBookBase {
public:
    bool                    cancelOrder(OrderId id) override;
    bool                    hasOrder(OrderId id) const override { return findSlot(id) != nullptr; }
    std::optional<Order>    getOrder(OrderId id) const override;
    std::optional<Price>    bestBid() const override;
    std::optional<Price>    bestAsk() const override;
    std::vector<DepthLevel> depth(Side side, std::size_t levels) const override;

protected:
    void match(Order& incoming, std::vector<Trade>& out) override;
    void rest(const Order& order) override;

private:
    // All orders at one price. Each order ever added to the level gets the next
    // sequence number; the order with sequence number s lives in orders[s - firstSeq].
    // Dropping dead slots from the front only has to increase firstSeq, so the index
    // entries of the orders behind them stay correct.
    struct Level {
        std::vector<Order> orders;        // arrival order; quantity 0 = dead slot
        std::size_t        head = 0;      // slot of the oldest live order
        std::size_t        live = 0;      // how many live orders
        std::uint64_t      firstSeq = 0;  // sequence number of orders[0]
    };

    // Where a resting order is.
    struct Slot {
        Price         price = 0;
        std::uint64_t seq = 0;
        Side          side = Side::Buy;
        bool          used = false;
    };

    std::vector<Level> bidLevels_, askLevels_;   // both cover prices basePrice_ .. basePrice_ + size - 1
    Price              basePrice_ = 0;

    std::size_t bidOrders_ = 0, askOrders_ = 0;  // live orders on each side
    Price       bestBid_ = 0, bestAsk_ = 0;      // only meaningful while that side has orders

    std::deque<Slot>                    direct_;     // direct_[id], for ids below kDirectLimit
    std::unordered_map<OrderId, Slot>   overflow_;   // every other id

    std::vector<Level>&       levels(Side side)       { return side == Side::Buy ? bidLevels_ : askLevels_; }
    const std::vector<Level>& levels(Side side) const { return side == Side::Buy ? bidLevels_ : askLevels_; }
    Level& levelAt(Side side, Price price) { return levels(side)[static_cast<std::size_t>(price - basePrice_)]; }

    void cover(Price price);               // grow the level arrays until they include this price
    void moveBest(Side side);              // the best level emptied: find the next one
    void tidy(Level& level);               // clean up after an order left the level
    void compact(Level& level);            // squeeze out the dead slots

    const Slot* findSlot(OrderId id) const;
    Slot&       slotFor(OrderId id);
    void        forget(OrderId id);
};
