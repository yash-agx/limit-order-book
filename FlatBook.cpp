#include "FlatBook.hpp"
#include <algorithm>

namespace {

const Price       kPadding     = 1024;        // spare price levels kept on each side of the book
const OrderId     kDirectLimit = 1u << 22;    // ids below this (about 4 million) use the array index
const std::size_t kSlack       = 32;          // don't bother cleaning a level for fewer dead slots than this

}  // namespace

// ---------- index: order id -> where the order is ----------

const FlatBook::Slot* FlatBook::findSlot(OrderId id) const {
    if (id < kDirectLimit) {
        if (id >= direct_.size() || !direct_[id].used) return nullptr;
        return &direct_[id];
    }
    auto found = overflow_.find(id);
    return found == overflow_.end() ? nullptr : &found->second;
}

FlatBook::Slot& FlatBook::slotFor(OrderId id) {
    if (id >= kDirectLimit) return overflow_[id];
    if (id >= direct_.size()) direct_.resize(id + 1);   // a deque grows in blocks: nothing already stored moves
    return direct_[id];
}

void FlatBook::forget(OrderId id) {
    if (id < kDirectLimit) direct_[id].used = false;
    else                   overflow_.erase(id);
}

// ---------- price levels ----------

void FlatBook::cover(Price price) {
    if (bidLevels_.empty()) {                            // first order ever: centre the arrays on it
        basePrice_ = price - kPadding;
        bidLevels_.resize(2 * kPadding);
        askLevels_.resize(2 * kPadding);
    } else if (price < basePrice_) {                     // extend downwards (rare: every level moves up)
        std::size_t extra = static_cast<std::size_t>(basePrice_ - price + kPadding);
        bidLevels_.insert(bidLevels_.begin(), extra, Level{});
        askLevels_.insert(askLevels_.begin(), extra, Level{});
        basePrice_ -= static_cast<Price>(extra);
    } else if (price - basePrice_ >= static_cast<Price>(bidLevels_.size())) {   // extend upwards
        std::size_t size = static_cast<std::size_t>(price - basePrice_ + kPadding);
        bidLevels_.resize(size);
        askLevels_.resize(size);
    }
}

// Called when the best level on a side has just emptied but the side still has orders:
// step one price at a time, away from the other side, until a level with orders turns up.
void FlatBook::moveBest(Side side) {
    if (side == Side::Buy) while (levelAt(Side::Buy,  bestBid_).live == 0) --bestBid_;
    else                   while (levelAt(Side::Sell, bestAsk_).live == 0) ++bestAsk_;
}

void FlatBook::tidy(Level& level) {
    if (level.live == 0) {                               // nothing left: start again from an empty vector
        level.firstSeq += level.orders.size();           // (clear() keeps the memory, ready for the next orders)
        level.orders.clear();
        level.head = 0;
        return;
    }

    while (level.orders[level.head].quantity == 0) ++level.head;         // head must point at a live order
    while (level.orders.back().quantity == 0) level.orders.pop_back();   // dead slots at the back just go

    std::size_t deadInMiddle = level.orders.size() - level.head - level.live;
    if (deadInMiddle > level.live + kSlack) {
        compact(level);
    } else if (level.head >= kSlack && level.head * 2 >= level.orders.size()) {
        // The front half is all dead: cut it off. firstSeq moves with it, so no index entry changes.
        level.orders.erase(level.orders.begin(), level.orders.begin() + static_cast<std::ptrdiff_t>(level.head));
        level.firstSeq += level.head;
        level.head = 0;
    }
}

// More dead slots than live orders: slide the live ones together (keeping their order)
// and tell the index where each one now is. Costs one index write per live order, but it
// only happens after at least that many cancels, so the cost per cancel stays small.
void FlatBook::compact(Level& level) {
    level.firstSeq += level.head;
    std::size_t write = 0;
    for (std::size_t read = level.head; read < level.orders.size(); ++read) {
        if (level.orders[read].quantity == 0) continue;
        level.orders[write] = level.orders[read];
        slotFor(level.orders[write].id).seq = level.firstSeq + write;
        ++write;
    }
    level.orders.resize(write);
    level.head = 0;
}

// ---------- the engine ----------

void FlatBook::match(Order& incoming, std::vector<Trade>& out) {
    Side         opposite = incoming.side == Side::Buy ? Side::Sell : Side::Buy;
    std::size_t& resting  = opposite == Side::Buy ? bidOrders_ : askOrders_;   // live orders on the other side
    Price&       best     = opposite == Side::Buy ? bestBid_ : bestAsk_;

    while (incoming.quantity > 0 && resting > 0 && crosses(incoming, best)) {
        Level& level = levelAt(opposite, best);
        while (incoming.quantity > 0 && level.live > 0) {
            Order&   oldest = level.orders[level.head];          // oldest order at this price goes first
            Quantity qty    = std::min(incoming.quantity, oldest.quantity);

            out.push_back(makeTrade(incoming, oldest.id, best, qty));

            incoming.quantity -= qty;
            oldest.quantity   -= qty;                            // reaching 0 also marks the slot dead

            if (oldest.quantity == 0) {
                forget(oldest.id);
                --level.live;
                --resting;
                tidy(level);
            }
        }
        if (level.live == 0 && resting > 0) moveBest(opposite);
    }
}

void FlatBook::rest(const Order& order) {
    cover(order.price);

    Level& level = levelAt(order.side, order.price);
    level.orders.push_back(order);                               // join the back of the queue
    ++level.live;

    Slot& slot = slotFor(order.id);
    slot = {order.price, level.firstSeq + level.orders.size() - 1, order.side, true};

    if (order.side == Side::Buy) {
        if (bidOrders_ == 0 || order.price > bestBid_) bestBid_ = order.price;
        ++bidOrders_;
    } else {
        if (askOrders_ == 0 || order.price < bestAsk_) bestAsk_ = order.price;
        ++askOrders_;
    }
}

bool FlatBook::cancelOrder(OrderId id) {
    const Slot* found = findSlot(id);
    if (!found) return false;
    Slot slot = *found;                                          // copy: forget() is about to clear it

    Level& level = levelAt(slot.side, slot.price);
    level.orders[slot.seq - level.firstSeq].quantity = 0;        // mark the slot dead
    --level.live;
    forget(id);
    tidy(level);

    std::size_t& resting = slot.side == Side::Buy ? bidOrders_ : askOrders_;
    --resting;
    Price best = slot.side == Side::Buy ? bestBid_ : bestAsk_;
    if (level.live == 0 && resting > 0 && slot.price == best) moveBest(slot.side);
    return true;
}

std::optional<Order> FlatBook::getOrder(OrderId id) const {
    const Slot* slot = findSlot(id);
    if (!slot) return std::nullopt;
    const Level& level = levels(slot->side)[static_cast<std::size_t>(slot->price - basePrice_)];
    return level.orders[slot->seq - level.firstSeq];
}

std::optional<Price> FlatBook::bestBid() const {
    if (bidOrders_ == 0) return std::nullopt;
    return bestBid_;
}

std::optional<Price> FlatBook::bestAsk() const {
    if (askOrders_ == 0) return std::nullopt;
    return bestAsk_;
}

std::vector<DepthLevel> FlatBook::depth(Side side, std::size_t maxLevels) const {
    std::vector<DepthLevel> out;
    std::size_t remaining = side == Side::Buy ? bidOrders_ : askOrders_;   // stop once every order is accounted for
    Price       price     = side == Side::Buy ? bestBid_ : bestAsk_;
    Price       step      = side == Side::Buy ? -1 : 1;                    // bids: walk down. asks: walk up.

    while (remaining > 0 && out.size() < maxLevels) {
        const Level& level = levels(side)[static_cast<std::size_t>(price - basePrice_)];
        if (level.live > 0) {
            Quantity total = 0;
            for (std::size_t i = level.head; i < level.orders.size(); ++i) total += level.orders[i].quantity;   // dead slots add 0
            out.push_back({price, total, level.live});
            remaining -= level.live;
        }
        price += step;
    }
    return out;
}
