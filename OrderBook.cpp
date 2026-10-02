#include "OrderBook.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>

namespace {

// Can this incoming order trade against a resting order at levelPrice?
bool crosses(const Order& o, Price levelPrice) {
    if (o.type == OrderType::Market) return true;            // market orders take any price
    return o.side == Side::Buy ? levelPrice <= o.price         // buyer: ask must be at or below my limit
                               : levelPrice >= o.price;        // seller: bid must be at or above my limit
}

// Walk one side of the book from the best price and summarise the first `levels` levels.
// A template so the same code works for bids_ and asks_ (they are different map types).
template <typename Book>
std::vector<DepthLevel> topLevels(const Book& book, std::size_t levels) {
    std::vector<DepthLevel> out;
    for (const auto& [price, level] : book) {
        if (out.size() == levels) break;
        Quantity total = 0;
        for (const Order& o : level) total += o.quantity;
        out.push_back({price, total, level.size()});
    }
    return out;
}

// Current time in nanoseconds. steady_clock only ever moves forward (unlike the wall clock,
// which can be adjusted), so it is the right clock for ordering events and measuring latency.
Timestamp nowNs() {
    auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count();
}

void printPrice(Price p) { std::printf("$%lld.%02lld", (long long)(p / 100), (long long)(p % 100)); }

}  // namespace

RejectReason OrderBook::validate(const Order& order) const {
    if (order.quantity == 0)        return RejectReason::ZeroQuantity;
    if (index_.count(order.id) > 0) return RejectReason::DuplicateId;   // id is already resting
    return RejectReason::None;
}

std::vector<Trade> OrderBook::addOrder(Order order) {
    std::vector<Trade> fills;
    if (validate(order) != RejectReason::None) return fills;   // rejected before it can touch the book

    order.timestamp = nowNs();   // the book stamps arrival time itself; whatever the caller put there is overwritten

    if (order.side == Side::Buy) match(order, asks_, fills);
    else                         match(order, bids_, fills);

    // Leftover limit quantity waits in the book. Leftover market quantity is dropped
    // (nothing left to trade against).
    if (order.quantity > 0 && order.type == OrderType::Limit) {
        if (order.side == Side::Buy) rest(order, bids_);
        else                         rest(order, asks_);
    }

    trades_.insert(trades_.end(), fills.begin(), fills.end());
    return fills;
}

template <typename Book>
void OrderBook::match(Order& incoming, Book& opposite, std::vector<Trade>& out) {
    while (incoming.quantity > 0 && !opposite.empty()) {
        auto best = opposite.begin();                  // best price level on the other side
        if (!crosses(incoming, best->first)) break;    // prices don't overlap -> stop

        Level& level = best->second;
        while (incoming.quantity > 0 && !level.empty()) {
            Order& resting = level.front();            // oldest order at this price goes first
            Quantity qty = std::min(incoming.quantity, resting.quantity);

            bool buyerIsIncoming = incoming.side == Side::Buy;
            out.push_back({buyerIsIncoming ? incoming.id : resting.id,
                           buyerIsIncoming ? resting.id  : incoming.id,
                           best->first, qty, incoming.timestamp});

            incoming.quantity -= qty;
            resting.quantity  -= qty;                  // partial fill if this stays > 0

            if (resting.quantity == 0) {
                index_.erase(resting.id);
                level.pop_front();
            }
        }
        if (level.empty()) opposite.erase(best);       // remove empty price levels
    }
}

template <typename Book>
void OrderBook::rest(const Order& order, Book& own) {
    Level& level = own[order.price];                   // creates the level if it doesn't exist
    level.push_back(order);                            // join the back of the queue
    index_[order.id] = {order.side, order.price, std::prev(level.end())};
}

bool OrderBook::cancelOrder(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;

    const Location& loc = found->second;
    if (loc.side == Side::Buy) eraseFrom(bids_, loc.price, loc.it);
    else                       eraseFrom(asks_, loc.price, loc.it);

    index_.erase(found);
    return true;
}

bool OrderBook::modifyOrder(OrderId id, Price newPrice, Quantity newQuantity) {
    auto found = index_.find(id);
    if (found == index_.end() || newQuantity == 0) return false;   // check first, so a failed modify changes nothing

    Order replacement = *found->second.it;             // copy it now: cancelOrder destroys the original
    replacement.price    = newPrice;
    replacement.quantity = newQuantity;

    cancelOrder(id);
    addOrder(replacement);                             // new timestamp, back of the queue
    return true;
}

template <typename Book>
void OrderBook::eraseFrom(Book& book, Price price, Level::iterator it) {
    auto level = book.find(price);
    level->second.erase(it);
    if (level->second.empty()) book.erase(level);
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;            // no bids -> no best bid
    return bids_.begin()->first;                       // map is sorted, so begin() is the highest bid
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;                       // lowest ask
}

std::optional<Price> OrderBook::spread() const {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    return *bestAsk() - *bestBid();
}

std::vector<DepthLevel> OrderBook::depth(Side side, std::size_t levels) const {
    return side == Side::Buy ? topLevels(bids_, levels) : topLevels(asks_, levels);
}

std::optional<Order> OrderBook::getOrder(OrderId id) const {
    auto found = index_.find(id);
    if (found == index_.end()) return std::nullopt;
    return *found->second.it;                          // follow the stored iterator to the order
}

Quantity OrderBook::quantityAt(Side side, Price price) const {
    auto total = [](const Level& lvl) {
        Quantity q = 0;
        for (const Order& o : lvl) q += o.quantity;
        return q;
    };
    if (side == Side::Buy) { auto it = bids_.find(price); return it == bids_.end() ? 0 : total(it->second); }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : total(it->second);
}

void OrderBook::print() const {
    std::printf("      PRICE      QTY\n");
    for (auto it = asks_.rbegin(); it != asks_.rend(); ++it) {   // asks: highest at top
        std::printf("ASK  ");
        printPrice(it->first);
        std::printf("  %5u\n", quantityAt(Side::Sell, it->first));
    }
    std::printf("---------------------\n");
    for (const auto& [price, level] : bids_) {
        std::printf("BID  ");
        printPrice(price);
        std::printf("  %5u\n", quantityAt(Side::Buy, price));
    }
}
