#pragma once
#include "OrderBook.hpp"
#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

// One thing a trader can ask the exchange to do.
enum class ActionType { Add, Cancel, Modify };

struct Action {
    ActionType type;
    Order      order;   // Add: the order to submit (limit or market).
                        // Cancel: only order.id is used.
                        // Modify: order.id, plus the new price and new quantity.
};

// Run one action against a book.
void apply(OrderBook& book, const Action& action);

struct FlowConfig {
    std::uint64_t seed           = 42;
    Price         midPrice       = 10000;  // $100.00: the starting mid; afterwards the mid follows the book
    Price         maxOffset      = 200;    // passive orders sit 1..maxOffset cents away from the mid
    Quantity      maxQuantity    = 100;    // order sizes are 1..maxQuantity
    std::size_t   targetBookSize = 1000;   // the generator keeps about this many orders resting

    // Out of every 100 actions, roughly how many are of each kind.
    // Whatever is left over is split between passive limit orders and cancels.
    int marketPercent     = 5;    // market orders
    int aggressivePercent = 5;    // limit orders priced through the mid, so they trade at once
    int modifyPercent     = 10;   // modify a resting order
};

// Produces a random but reproducible stream of actions: the same seed always gives
// exactly the same stream.
//
// It runs every action through its own private OrderBook, so it always knows which
// orders are still resting. That means every cancel and modify it generates targets
// a live order, and it can hold the book near targetBookSize.
class OrderFlowGenerator {
public:
    explicit OrderFlowGenerator(const FlowConfig& config);

    // Passive limit orders that fill an empty book up to targetBookSize. None of them trade.
    std::vector<Action> prefill();

    Action              next();                        // one action from the normal mix
    std::vector<Action> generate(std::size_t count);   // `count` calls to next()

    std::size_t bookSize() const { return live_.size(); }   // orders resting right now

private:
    FlowConfig      config_;
    std::mt19937_64 rng_;          // the random number generator; its whole state comes from the seed
    Price           mid_;
    OrderId         nextId_ = 1;

    OrderBook book_;               // private copy of the book, used only to know what is resting
    std::vector<OrderId>                     live_;   // ids of resting orders, so one can be picked at random
    std::unordered_map<OrderId, std::size_t> pos_;    // id -> its position in live_

    std::uint64_t uniform(std::uint64_t lo, std::uint64_t hi);   // random whole number in [lo, hi]
    Side          randomSide();
    Quantity      randomQuantity();
    Price         passivePrice(Side side);
    OrderId       randomLiveId();

    Action passiveLimit();
    void   record(const Action& action);
    void   sync(OrderId id);
};
