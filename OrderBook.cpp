#include "OrderBook.hpp"
#include <algorithm>
#include <iterator>

namespace {

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

// A memory resource that is nothing more than plain new/delete, exactly what std::map,
// std::list and std::unordered_map do by default.
// (The standard library's own std::pmr::new_delete_resource() isn't used because it goes
// through the "aligned" version of new, which on this compiler costs about 16 extra bytes
// per allocation - that would make the baseline look worse than ordinary containers.)
class PlainNewDelete : public std::pmr::memory_resource {
    void* do_allocate(std::size_t bytes, std::size_t) override { return ::operator new(bytes); }
    void  do_deallocate(void* p, std::size_t, std::size_t) override { ::operator delete(p); }
    bool  do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

std::pmr::memory_resource* plainNewDelete() {
    static PlainNewDelete resource;
    return &resource;
}

}  // namespace

OrderBook::OrderBook(bool usePool)
    : pool_(usePool ? std::make_unique<std::pmr::unsynchronized_pool_resource>() : nullptr),
      bids_(memory()), asks_(memory()), index_(memory()) {}

std::pmr::memory_resource* OrderBook::memory() const {
    if (pool_) return pool_.get();
    return plainNewDelete();
}

void OrderBook::match(Order& incoming, std::vector<Trade>& out) {
    if (incoming.side == Side::Buy) matchAgainst(incoming, asks_, out);
    else                            matchAgainst(incoming, bids_, out);
}

void OrderBook::rest(const Order& order) {
    if (order.side == Side::Buy) restIn(order, bids_);
    else                         restIn(order, asks_);
}

template <typename Book>
void OrderBook::matchAgainst(Order& incoming, Book& opposite, std::vector<Trade>& out) {
    while (incoming.quantity > 0 && !opposite.empty()) {
        auto best = opposite.begin();                  // best price level on the other side
        if (!crosses(incoming, best->first)) break;    // prices don't overlap -> stop

        Level& level = best->second;
        while (incoming.quantity > 0 && !level.empty()) {
            Order& resting = level.front();            // oldest order at this price goes first
            Quantity qty = std::min(incoming.quantity, resting.quantity);

            out.push_back(makeTrade(incoming, resting.id, best->first, qty));

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
void OrderBook::restIn(const Order& order, Book& own) {
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

std::vector<DepthLevel> OrderBook::depth(Side side, std::size_t levels) const {
    return side == Side::Buy ? topLevels(bids_, levels) : topLevels(asks_, levels);
}

std::optional<Order> OrderBook::getOrder(OrderId id) const {
    auto found = index_.find(id);
    if (found == index_.end()) return std::nullopt;
    return *found->second.it;                          // follow the stored iterator to the order
}
