#pragma once
#include "Order.hpp"
#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

// The interface every matching engine implements.
//
// Tests, the order-flow generator and the benchmark only ever talk to an OrderBookBase,
// so they work with any engine without changes.
//
// The rules that must be identical in every engine (rejecting bad orders, stamping the
// arrival time, recording trades, modify = cancel + re-add) are written once here.
// An engine only supplies the parts that depend on its data structures: how to match an
// incoming order, how to store a resting order, how to cancel one, and how to look things up.
class OrderBookBase {
public:
    virtual ~OrderBookBase() = default;

    // ---------- the same for every engine (OrderBookBase.cpp) ----------

    // Submit an order. Returns the trades it caused (empty if nothing matched).
    // A rejected order (see validate) is ignored: no trades, and the book is unchanged.
    std::vector<Trade> addOrder(Order order);

    // Would addOrder accept this order? Rejects zero quantity, and ids that are
    // already resting in the book.
    RejectReason validate(const Order& order) const;

    // Change a resting order's price and/or quantity. Done as cancel + re-add, so the order
    // goes to the back of the queue (loses time priority) and may trade if the new price crosses;
    // those trades appear in tradeHistory(). Returns false, changing nothing, if the id isn't
    // in the book or newQuantity is 0.
    bool modifyOrder(OrderId id, Price newPrice, Quantity newQuantity);

    std::optional<Price> spread() const;   // bestAsk - bestBid; empty unless both sides have orders
    Quantity quantityAt(Side side, Price price) const;
    void print() const;

    // Every trade since the book was created, oldest first.
    const std::vector<Trade>& tradeHistory() const { return trades_; }

    // Make room for `count` trades now, so recording a trade never has to stop and
    // copy the whole history into a bigger block of memory.
    void reserveTrades(std::size_t count);

    // ---------- what each engine must provide ----------

    // Remove a resting order. Returns false if the id isn't in the book.
    virtual bool cancelOrder(OrderId id) = 0;

    virtual bool                 hasOrder(OrderId id) const = 0;
    virtual std::optional<Order> getOrder(OrderId id) const = 0;   // a copy of a resting order, if it exists

    // Top of book. Empty optional if that side has no orders.
    virtual std::optional<Price> bestBid() const = 0;
    virtual std::optional<Price> bestAsk() const = 0;

    // Market depth: the best `levels` price levels on one side, best price first.
    virtual std::vector<DepthLevel> depth(Side side, std::size_t levels) const = 0;

    // A hint: about this many orders will rest in the book. Engines that can prepare
    // memory in advance do so; the others ignore it.
    virtual void reserveOrders(std::size_t expectedOrders) { (void)expectedOrders; }

protected:
    // Trade `incoming` against the other side of the book for as long as prices cross.
    // Must reduce incoming.quantity and append one Trade per fill to `out`.
    virtual void match(Order& incoming, std::vector<Trade>& out) = 0;

    // Put what is left of a limit order into the book, at the back of its price level.
    virtual void rest(const Order& order) = 0;

    // Can this incoming order trade against a resting order at levelPrice?
    static bool crosses(const Order& incoming, Price levelPrice) {
        if (incoming.type == OrderType::Market) return true;                // market orders take any price
        return incoming.side == Side::Buy ? levelPrice <= incoming.price     // buyer: ask must be at or below my limit
                                          : levelPrice >= incoming.price;    // seller: bid must be at or above my limit
    }

    // Trades happen at the resting order's price and carry the incoming order's arrival time.
    static Trade makeTrade(const Order& incoming, OrderId restingId, Price price, Quantity quantity) {
        bool buyerIsIncoming = incoming.side == Side::Buy;
        return {buyerIsIncoming ? incoming.id : restingId,
                buyerIsIncoming ? restingId   : incoming.id,
                price, quantity, incoming.timestamp};
    }

private:
    std::vector<Trade> trades_;
};
