#include "OrderFlow.hpp"
#include <algorithm>

void apply(OrderBook& book, const Action& action) {
    switch (action.type) {
        case ActionType::Add:    book.addOrder(action.order); break;
        case ActionType::Cancel: book.cancelOrder(action.order.id); break;
        case ActionType::Modify: book.modifyOrder(action.order.id, action.order.price, action.order.quantity); break;
    }
}

OrderFlowGenerator::OrderFlowGenerator(const FlowConfig& config)
    : config_(config), rng_(config.seed), mid_(config.midPrice) {}

// The standard library's distribution classes (std::uniform_int_distribution) may give
// different numbers on different compilers. Doing the arithmetic ourselves keeps the
// order flow identical everywhere. (% is very slightly biased; that doesn't matter here.)
std::uint64_t OrderFlowGenerator::uniform(std::uint64_t lo, std::uint64_t hi) {
    return lo + rng_() % (hi - lo + 1);
}

Side OrderFlowGenerator::randomSide() { return uniform(0, 1) == 0 ? Side::Buy : Side::Sell; }

Quantity OrderFlowGenerator::randomQuantity() {
    return static_cast<Quantity>(uniform(1, config_.maxQuantity));
}

// A price on the order's own side of the mid, so it normally rests instead of trading.
// Taking the smaller of two random numbers makes small offsets more likely: most orders
// sit close to the mid and the book gets thinner further away, like a real one.
Price OrderFlowGenerator::passivePrice(Side side) {
    std::uint64_t range  = static_cast<std::uint64_t>(config_.maxOffset);
    Price         offset = 1 + static_cast<Price>(std::min(uniform(0, range - 1), uniform(0, range - 1)));
    return side == Side::Buy ? mid_ - offset : mid_ + offset;
}

OrderId OrderFlowGenerator::randomLiveId() { return live_[uniform(0, live_.size() - 1)]; }

Action OrderFlowGenerator::passiveLimit() {
    Side side = randomSide();
    return {ActionType::Add, {nextId_++, side, OrderType::Limit, passivePrice(side), randomQuantity()}};
}

std::vector<Action> OrderFlowGenerator::prefill() {
    std::vector<Action> out;
    while (live_.size() < config_.targetBookSize) {
        out.push_back(passiveLimit());
        record(out.back());
    }
    return out;
}

Action OrderFlowGenerator::next() {
    // The mid price drifts: about 2 actions in 100 move it up or down by one cent.
    if (uniform(1, 100) <= 2) mid_ += (uniform(0, 1) == 0 ? -1 : 1);
    mid_ = std::max(mid_, config_.maxOffset + 1);      // keep every price above zero

    int roll        = static_cast<int>(uniform(1, 100));
    int marketUpTo  = config_.marketPercent;
    int aggressUpTo = marketUpTo + config_.aggressivePercent;
    int modifyUpTo  = aggressUpTo + config_.modifyPercent;

    Action action;
    if (roll <= marketUpTo) {
        action = {ActionType::Add, {nextId_++, randomSide(), OrderType::Market, 0, randomQuantity()}};
    } else if (roll <= aggressUpTo) {
        // A buy priced above the mid (or a sell below it) reaches into the other side and trades.
        Side  side    = randomSide();
        Price through = static_cast<Price>(uniform(1, 5));
        Price price   = side == Side::Buy ? mid_ + through : mid_ - through;
        action = {ActionType::Add, {nextId_++, side, OrderType::Limit, price, randomQuantity()}};
    } else if (roll <= modifyUpTo && !live_.empty()) {
        OrderId id   = randomLiveId();
        Side    side = book_.getOrder(id)->side;
        action = {ActionType::Modify, {id, side, OrderType::Limit, passivePrice(side), randomQuantity()}};
    } else if (live_.size() >= config_.targetBookSize && !live_.empty()) {
        // Book is at its target size: cancel something instead of adding.
        // This is what stops the book growing or shrinking over a long run.
        Order cancelled{};
        cancelled.id = randomLiveId();
        action = {ActionType::Cancel, cancelled};
    } else {
        action = passiveLimit();
    }

    record(action);
    return action;
}

std::vector<Action> OrderFlowGenerator::generate(std::size_t count) {
    std::vector<Action> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) out.push_back(next());
    return out;
}

// Run the action on the private book, then bring live_ up to date for every order it touched:
// the order itself, plus both sides of any trade it caused.
void OrderFlowGenerator::record(const Action& action) {
    std::size_t tradesBefore = book_.tradeHistory().size();
    apply(book_, action);

    sync(action.order.id);
    const std::vector<Trade>& history = book_.tradeHistory();
    for (std::size_t i = tradesBefore; i < history.size(); ++i) {
        sync(history[i].buyId);
        sync(history[i].sellId);
    }
}

// Make live_ agree with the book about one order id.
void OrderFlowGenerator::sync(OrderId id) {
    bool resting = book_.hasOrder(id);
    auto known   = pos_.find(id);

    if (resting && known == pos_.end()) {
        pos_[id] = live_.size();
        live_.push_back(id);
    } else if (!resting && known != pos_.end()) {
        // Remove from the middle of a vector in O(1): move the last id into the gap, drop the end.
        std::size_t gap = known->second;
        pos_[live_.back()] = gap;
        live_[gap] = live_.back();
        live_.pop_back();
        pos_.erase(id);
    }
}
