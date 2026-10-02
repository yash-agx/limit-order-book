#pragma once
#include "Order.hpp"
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

class OrderBook {
public:
    // Submit an order. Returns the trades it caused (empty if nothing matched).
    // A rejected order (see validate) is ignored: no trades, and the book is unchanged.
    std::vector<Trade> addOrder(Order order);

    // Would addOrder accept this order? Rejects zero quantity, and ids that are
    // already resting in the book.
    RejectReason validate(const Order& order) const;

    // Remove a resting order. Returns false if the id isn't in the book.
    bool cancelOrder(OrderId id);

    // Change a resting order's price and/or quantity. Done as cancel + re-add, so the order
    // goes to the back of the queue (loses time priority) and may trade if the new price crosses;
    // those trades appear in tradeHistory(). Returns false, changing nothing, if the id isn't
    // in the book or newQuantity is 0.
    bool modifyOrder(OrderId id, Price newPrice, Quantity newQuantity);

    // Top of book. Empty optional if that side has no orders.
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    std::optional<Price> spread() const;   // bestAsk - bestBid; empty unless both sides have orders

    // Market depth: the best `levels` price levels on one side, best price first.
    std::vector<DepthLevel> depth(Side side, std::size_t levels) const;

    void print() const;
    // Every trade since the book was created, oldest first.
    const std::vector<Trade>& tradeHistory() const { return trades_; }

    // Small helpers, mostly for tests
    bool     hasOrder(OrderId id) const { return index_.count(id) > 0; }
    std::optional<Order> getOrder(OrderId id) const;   // a copy of a resting order, if it exists
    Quantity quantityAt(Side side, Price price) const;

private:
    // All orders at one price, oldest first (time priority).
    // std::list because erasing from the middle (a cancel) is O(1) and doesn't break other iterators.
    using Level = std::list<Order>;

    std::map<Price, Level, std::greater<Price>> bids_;  // highest price first
    std::map<Price, Level>                      asks_;  // lowest price first

    // Lets cancelOrder jump straight to an order instead of searching the whole book.
    struct Location {
        Side            side;
        Price           price;
        Level::iterator it;
    };
    std::unordered_map<OrderId, Location> index_;

    std::vector<Trade> trades_;

    template <typename Book>
    void match(Order& incoming, Book& opposite, std::vector<Trade>& out);

    template <typename Book>
    void rest(const Order& order, Book& own);

    template <typename Book>
    static void eraseFrom(Book& book, Price price, Level::iterator it);
};
