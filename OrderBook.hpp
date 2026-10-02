#pragma once
#include "OrderBookBase.hpp"
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <memory_resource>
#include <unordered_map>

// Implementation A - the baseline, built from standard containers:
//   std::map            price -> level, kept sorted so begin() is always the best price
//   std::list           the queue of orders at one price, oldest first
//   std::unordered_map  order id -> where that order is, for O(1) cancel
//
// The containers are the std::pmr versions. They are the same containers; the only
// difference is that they ask a "memory resource" object for memory instead of calling
// new/delete directly. That lets one class serve two experiments:
//   usePool = false : plain new/delete for every node  (implementation A)
//   usePool = true  : nodes come from a memory pool     (ablation step C1)
// so the pool is the only thing that changes between the two.
class OrderBook : public OrderBookBase {
public:
    explicit OrderBook(bool usePool = false);

    bool                    cancelOrder(OrderId id) override;
    bool                    hasOrder(OrderId id) const override { return index_.count(id) > 0; }
    std::optional<Order>    getOrder(OrderId id) const override;
    std::optional<Price>    bestBid() const override;
    std::optional<Price>    bestAsk() const override;
    std::vector<DepthLevel> depth(Side side, std::size_t levels) const override;
    void                    reserveOrders(std::size_t expectedOrders) override { index_.reserve(expectedOrders); }

protected:
    void match(Order& incoming, std::vector<Trade>& out) override;
    void rest(const Order& order) override;

private:
    // Declared first so it is created before the containers and destroyed after them.
    std::unique_ptr<std::pmr::unsynchronized_pool_resource> pool_;
    std::pmr::memory_resource* memory() const;

    // All orders at one price, oldest first (time priority).
    // std::list because erasing from the middle (a cancel) is O(1) and doesn't break other iterators.
    using Level = std::pmr::list<Order>;

    std::pmr::map<Price, Level, std::greater<Price>> bids_;  // highest price first
    std::pmr::map<Price, Level>                      asks_;  // lowest price first

    // Lets cancelOrder jump straight to an order instead of searching the whole book.
    struct Location {
        Side            side;
        Price           price;
        Level::iterator it;
    };
    std::pmr::unordered_map<OrderId, Location> index_;

    template <typename Book>
    void matchAgainst(Order& incoming, Book& opposite, std::vector<Trade>& out);

    template <typename Book>
    void restIn(const Order& order, Book& own);

    template <typename Book>
    static void eraseFrom(Book& book, Price price, Level::iterator it);
};

// Ablation step C1: exactly engine A, except every map, list and hash-map node comes from
// a memory pool (std::pmr::unsynchronized_pool_resource). The pool takes big blocks from
// the system and cuts them into same-sized pieces, so getting or returning a node is a
// few instructions instead of a trip through the general-purpose allocator.
class PoolBook : public OrderBook {
public:
    PoolBook() : OrderBook(true) {}
};
