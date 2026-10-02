#include "OrderBookBase.hpp"
#include <chrono>
#include <cstdio>

namespace {

// Current time in nanoseconds. steady_clock only ever moves forward (unlike the wall clock,
// which can be adjusted), so it is the right clock for ordering events and measuring latency.
Timestamp nowNs() {
    auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count();
}

void printPrice(Price p) { std::printf("$%lld.%02lld", (long long)(p / 100), (long long)(p % 100)); }

const std::size_t kAllLevels = static_cast<std::size_t>(-1);

}  // namespace

RejectReason OrderBookBase::validate(const Order& order) const {
    if (order.quantity == 0) return RejectReason::ZeroQuantity;
    if (hasOrder(order.id))  return RejectReason::DuplicateId;   // id is already resting
    return RejectReason::None;
}

std::vector<Trade> OrderBookBase::addOrder(Order order) {
    std::vector<Trade> fills;
    if (validate(order) != RejectReason::None) return fills;   // rejected before it can touch the book

    order.timestamp = nowNs();   // the book stamps arrival time itself; whatever the caller put there is overwritten

    match(order, fills);

    // Leftover limit quantity waits in the book. Leftover market quantity is dropped
    // (nothing left to trade against).
    if (order.quantity > 0 && order.type == OrderType::Limit) rest(order);

    trades_.insert(trades_.end(), fills.begin(), fills.end());
    return fills;
}

bool OrderBookBase::modifyOrder(OrderId id, Price newPrice, Quantity newQuantity) {
    std::optional<Order> current = getOrder(id);
    if (!current || newQuantity == 0) return false;    // check first, so a failed modify changes nothing

    Order replacement = *current;                      // a copy: cancelOrder destroys the original
    replacement.price    = newPrice;
    replacement.quantity = newQuantity;

    cancelOrder(id);
    addOrder(replacement);                             // new timestamp, back of the queue
    return true;
}

std::optional<Price> OrderBookBase::spread() const {
    std::optional<Price> bid = bestBid(), ask = bestAsk();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}

Quantity OrderBookBase::quantityAt(Side side, Price price) const {
    for (const DepthLevel& level : depth(side, kAllLevels))
        if (level.price == price) return level.quantity;
    return 0;
}

void OrderBookBase::reserveTrades(std::size_t count) {
    // reserve() on its own only books the memory: Windows hands over each page the first
    // time it is written, and that would show up as random slow actions later. Growing the
    // vector to full size and shrinking it back writes every slot once, now. Shrinking a
    // vector keeps its memory, so the space stays ready.
    std::size_t have = trades_.size();
    if (count <= have) return;
    trades_.resize(count);
    trades_.resize(have);
}

void OrderBookBase::print() const {
    std::vector<DepthLevel> asks = depth(Side::Sell, kAllLevels);
    std::printf("      PRICE      QTY\n");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {     // asks: highest at top
        std::printf("ASK  ");
        printPrice(it->price);
        std::printf("  %5u\n", it->quantity);
    }
    std::printf("---------------------\n");
    for (const DepthLevel& level : depth(Side::Buy, kAllLevels)) {
        std::printf("BID  ");
        printPrice(level.price);
        std::printf("  %5u\n", level.quantity);
    }
}
