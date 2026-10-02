#pragma once
#include "OrderBookBase.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <memory_resource>
#include <unordered_map>
#include <vector>

// Implementation C - custom, low-level data structures. Built up in steps so each
// optimisation can be measured on its own (the "ablation"):
//
//   C2 = pool + intrusive list
//   C3 = C2 + open-addressing hash map for the id index
//   C4 = C3 + cache-friendly node layout   (this is the complete engine C)
//   D  = C4 with engine B's flat price-level array in place of the std::map
//
// Shared by every step:
//   - All orders live in ONE pre-allocated array, the pool. An order is referred to by its
//     position in that array (a 32-bit "node index"), not by a pointer.
//   - The queue at each price is an intrusive doubly-linked list: the prev/next links are
//     stored inside the order itself, so there is no separate list node to allocate.
//
// The class is a template with three interchangeable parts, so the engine code below is
// written once and the steps differ only in which parts are plugged in:
//   Index    - finds an order's node index from its id
//   Pool     - stores the nodes
//   LevelSet - finds the price level for a price, and knows which level is best

const std::uint32_t kNoNode = 0xFFFFFFFFu;   // "no node": plays the role of a null pointer

// ---------------------------------------------------------------------------------------
// Index, version 1: the standard hash map (its nodes come from a memory pool).
// ---------------------------------------------------------------------------------------
class StdIndex {
public:
    void reserve(std::size_t orders) { map_.reserve(orders); }

    std::uint32_t find(OrderId id) const {
        auto found = map_.find(id);
        return found == map_.end() ? kNoNode : found->second;
    }
    void insert(OrderId id, std::uint32_t node) { map_[id] = node; }
    void erase(OrderId id) { map_.erase(id); }

private:
    std::pmr::unsynchronized_pool_resource             memory_;        // declared first: outlives the map
    std::pmr::unordered_map<OrderId, std::uint32_t>    map_{&memory_};
};

// ---------------------------------------------------------------------------------------
// Index, version 2: an open-addressing hash map.
//
// The standard hash map keeps every entry in its own separately allocated node and reaches
// it by following pointers. Here all entries sit in one flat array of 16-byte slots. An id
// is hashed to a "home" slot; if that slot is taken, the entry goes in the next free slot
// along (linear probing). A lookup starts at the home slot and walks forward until it finds
// the id or hits an empty slot - usually within the same cache line.
// ---------------------------------------------------------------------------------------
class OpenIndex {
public:
    OpenIndex() { resize(16); }

    void reserve(std::size_t orders) {
        std::size_t capacity = slots_.size();
        while (capacity < orders * 2) capacity *= 2;        // keep the table at most half full
        if (capacity != slots_.size()) resize(capacity);
    }

    std::uint32_t find(OrderId id) const {
        for (std::size_t i = home(id); slots_[i].node != kNoNode; i = (i + 1) & mask_)
            if (slots_[i].id == id) return slots_[i].node;
        return kNoNode;
    }

    void insert(OrderId id, std::uint32_t node) {
        if ((count_ + 1) * 2 > slots_.size()) resize(slots_.size() * 2);
        place(id, node);
        ++count_;
    }

    // Removing an entry must not leave a gap that would cut a later lookup short. So after
    // emptying the slot, walk forward and pull back any entry that had been pushed past
    // its home slot by the one just removed ("backward-shift deletion").
    void erase(OrderId id) {
        std::size_t gap = home(id);
        while (slots_[gap].id != id || slots_[gap].node == kNoNode) gap = (gap + 1) & mask_;

        for (std::size_t next = (gap + 1) & mask_; slots_[next].node != kNoNode; next = (next + 1) & mask_) {
            std::size_t wants = home(slots_[next].id);
            // The entry at `next` may move into the gap unless its home slot lies after the
            // gap and at or before `next` (counting round the end of the array).
            bool homeBetween = gap <= next ? (wants > gap && wants <= next)
                                           : (wants > gap || wants <= next);
            if (!homeBetween) {
                slots_[gap] = slots_[next];
                gap = next;
            }
        }
        slots_[gap].node = kNoNode;
        --count_;
    }

private:
    struct Slot {
        OrderId       id   = 0;
        std::uint32_t node = kNoNode;      // kNoNode marks an empty slot
    };

    std::vector<Slot> slots_;              // the size is always a power of two
    std::size_t       mask_  = 0;          // size - 1: "& mask_" wraps an index round the end
    int               shift_ = 0;
    std::size_t       count_ = 0;

    // Multiplying by a large odd number scrambles the id; the top bits of the result are the
    // best mixed, so those are the ones kept. Order ids are often 1, 2, 3, ... and this
    // spreads them evenly across the table.
    std::size_t home(OrderId id) const {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void place(OrderId id, std::uint32_t node) {
        std::size_t i = home(id);
        while (slots_[i].node != kNoNode) i = (i + 1) & mask_;
        slots_[i] = {id, node};
    }

    void resize(std::size_t capacity) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(capacity, Slot{});
        mask_  = capacity - 1;
        shift_ = 64;
        for (std::size_t c = capacity; c > 1; c /= 2) --shift_;   // shift_ = 64 - log2(capacity)
        for (const Slot& s : old)
            if (s.node != kNoNode) place(s.id, s.node);
    }
};

// ---------------------------------------------------------------------------------------
// Pool, version 1: each node is the order's fields plus the two list links. 48 bytes.
// ---------------------------------------------------------------------------------------
struct FatNode {
    OrderId       id;
    Side          side;
    Price         price;
    Quantity      quantity;
    Timestamp     timestamp;
    std::uint32_t prev;        // node index of the order ahead of this one at the same price
    std::uint32_t next;        // node index of the order behind it
};

// A pool hands out nodes and takes them back. Returned nodes are remembered in a "free
// list" (threaded through their `next` field) and reused before the array grows, so a
// running book stops asking the system for memory altogether.
class FatPool {
public:
    void reserve(std::size_t orders) { nodes_.reserve(orders); }

    std::uint32_t allocate() {
        if (firstFree_ != kNoNode) {                       // reuse the most recently freed node
            std::uint32_t n = firstFree_;
            firstFree_ = nodes_[n].next;
            return n;
        }
        nodes_.emplace_back();                             // none free: take a new one from the end
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }
    void release(std::uint32_t n) {
        nodes_[n].next = firstFree_;
        firstFree_ = n;
    }

    FatNode&       operator[](std::uint32_t n)       { return nodes_[n]; }
    const FatNode& operator[](std::uint32_t n) const { return nodes_[n]; }

    Timestamp& timestamp(std::uint32_t n)       { return nodes_[n].timestamp; }
    Timestamp  timestamp(std::uint32_t n) const { return nodes_[n].timestamp; }

private:
    std::vector<FatNode> nodes_;
    std::uint32_t        firstFree_ = kNoNode;
};

// ---------------------------------------------------------------------------------------
// Pool, version 2: a cache-friendly layout.
//
// The CPU loads memory in 64-byte "cache lines". A 48-byte FatNode often lies across two
// of them, so touching one order can cost two trips to memory. SlimNode keeps only the
// fields that matching and cancelling actually use, in exactly 32 bytes, and alignas(32)
// makes every node start on a 32-byte boundary: two nodes fill one cache line exactly and
// no node ever straddles two. The timestamp is "cold" (stored but never read while
// matching), so it moves out to a separate array at the same position.
// ---------------------------------------------------------------------------------------
struct alignas(32) SlimNode {
    OrderId       id;          // 8 bytes
    Price         price;       // 8
    Quantity      quantity;    // 4
    std::uint32_t prev;        // 4
    std::uint32_t next;        // 4
    Side          side;        // 4
};
static_assert(sizeof(SlimNode) == 32, "SlimNode must be exactly half a cache line");

class SlimPool {
public:
    void reserve(std::size_t orders) {
        nodes_.reserve(orders);
        timestamps_.reserve(orders);
    }

    std::uint32_t allocate() {
        if (firstFree_ != kNoNode) {
            std::uint32_t n = firstFree_;
            firstFree_ = nodes_[n].next;
            return n;
        }
        nodes_.emplace_back();
        timestamps_.emplace_back();                        // the cold array grows in step
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }
    void release(std::uint32_t n) {
        nodes_[n].next = firstFree_;
        firstFree_ = n;
    }

    SlimNode&       operator[](std::uint32_t n)       { return nodes_[n]; }
    const SlimNode& operator[](std::uint32_t n) const { return nodes_[n]; }

    Timestamp& timestamp(std::uint32_t n)       { return timestamps_[n]; }
    Timestamp  timestamp(std::uint32_t n) const { return timestamps_[n]; }

private:
    std::vector<SlimNode>  nodes_;         // hot: read and written on every match and cancel
    std::vector<Timestamp> timestamps_;    // cold: written once when the order arrives
    std::uint32_t          firstFree_ = kNoNode;
};

// ---------------------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------------------
// One price level: just the two ends of its linked list, and how long it is.
struct ListLevel {
    std::uint32_t head  = kNoNode;   // oldest order
    std::uint32_t tail  = kNoNode;   // newest order
    std::uint32_t count = 0;
};

// ---------------------------------------------------------------------------------------
// Price levels for one side of the book, version 1: a sorted std::map (pool-allocated).
// `Compare` decides which price is "best": std::greater for bids, std::less for asks.
// ---------------------------------------------------------------------------------------
template <typename Compare>
class MapLevels {
public:
    bool       empty() const     { return map_.empty(); }
    Price      bestPrice() const { return map_.begin()->first; }
    ListLevel& best()            { return map_.begin()->second; }

    ListLevel& get(Price price)  { return map_[price]; }               // creates the level if it doesn't exist
    ListLevel& find(Price price) { return map_.find(price)->second; }  // the level must exist
    void       levelEmptied(Price price) { map_.erase(price); }        // its last order just left

    // Call visit(price, level) for each level, best price first, until it returns false.
    template <typename Visit>
    void forEach(Visit visit) const {
        for (const auto& [price, level] : map_)
            if (!visit(price, level)) return;
    }

private:
    std::pmr::unsynchronized_pool_resource   memory_;        // for the map nodes; declared first
    std::pmr::map<Price, ListLevel, Compare> map_{&memory_};
};

// ---------------------------------------------------------------------------------------
// Price levels for one side of the book, version 2: engine B's idea - a flat array indexed
// by price. levels_[price - base_] is the level for that price, so reaching a level is one
// subtraction instead of a walk down a tree. The best price is remembered, and when the
// best level empties we step along the array to the next level that has orders.
// ---------------------------------------------------------------------------------------
template <bool IsBidSide>
class FlatLevels {
public:
    bool       empty() const     { return occupied_ == 0; }
    Price      bestPrice() const { return best_; }
    ListLevel& best()            { return at(best_); }

    // The level a new order is about to join (so an empty level counts as occupied from now).
    ListLevel& get(Price price) {
        cover(price);
        ListLevel& level = at(price);
        if (level.count == 0) {
            if (occupied_ == 0 || isBetter(price, best_)) best_ = price;
            ++occupied_;
        }
        return level;
    }
    ListLevel& find(Price price) { return at(price); }

    void levelEmptied(Price price) {
        --occupied_;
        if (occupied_ > 0 && price == best_)
            while (at(best_).count == 0) best_ += kAway;   // some level still has orders, so this stops
    }

    template <typename Visit>
    void forEach(Visit visit) const {
        std::size_t left = occupied_;
        for (Price price = best_; left > 0; price += kAway) {
            const ListLevel& level = levels_[static_cast<std::size_t>(price - base_)];
            if (level.count == 0) continue;
            --left;
            if (!visit(price, level)) return;
        }
    }

private:
    static constexpr Price kAway    = IsBidSide ? -1 : 1;   // the direction that leads away from the other side
    static constexpr Price kPadding = 1024;                 // spare levels kept beyond the prices in use

    static bool isBetter(Price a, Price b) { return IsBidSide ? a > b : a < b; }

    std::vector<ListLevel> levels_;      // covers prices base_ .. base_ + size - 1
    Price                  base_ = 0;
    Price                  best_ = 0;    // only meaningful while occupied_ > 0
    std::size_t            occupied_ = 0;   // how many levels have at least one order

    ListLevel& at(Price price) { return levels_[static_cast<std::size_t>(price - base_)]; }

    void cover(Price price) {            // grow the array until it includes this price
        if (levels_.empty()) {
            base_ = price - kPadding;
            levels_.resize(static_cast<std::size_t>(2 * kPadding));
        } else if (price < base_) {      // extend downwards (rare: every level moves up)
            std::size_t extra = static_cast<std::size_t>(base_ - price + kPadding);
            levels_.insert(levels_.begin(), extra, ListLevel{});
            base_ -= static_cast<Price>(extra);
        } else if (price - base_ >= static_cast<Price>(levels_.size())) {
            levels_.resize(static_cast<std::size_t>(price - base_ + kPadding));
        }
    }
};

// Which kind of levels the two sides of a book use.
struct MapLevelSet {
    using Bids = MapLevels<std::greater<Price>>;   // highest price first
    using Asks = MapLevels<std::less<Price>>;      // lowest price first
};
struct FlatLevelSet {
    using Bids = FlatLevels<true>;
    using Asks = FlatLevels<false>;
};

template <typename Index, typename Pool, typename LevelSet>
class IntrusiveBook : public OrderBookBase {
public:
    bool cancelOrder(OrderId id) override {
        std::uint32_t n = index_.find(id);
        if (n == kNoNode) return false;

        if (pool_[n].side == Side::Buy) removeFrom(bids_, n);
        else                            removeFrom(asks_, n);
        return true;
    }

    bool hasOrder(OrderId id) const override { return index_.find(id) != kNoNode; }

    std::optional<Order> getOrder(OrderId id) const override {
        std::uint32_t n = index_.find(id);
        if (n == kNoNode) return std::nullopt;
        const auto& node = pool_[n];                       // only limit orders ever rest in the book
        return Order{node.id, node.side, OrderType::Limit, node.price, node.quantity, pool_.timestamp(n)};
    }

    std::optional<Price> bestBid() const override {
        if (bids_.empty()) return std::nullopt;
        return bids_.bestPrice();
    }
    std::optional<Price> bestAsk() const override {
        if (asks_.empty()) return std::nullopt;
        return asks_.bestPrice();
    }

    std::vector<DepthLevel> depth(Side side, std::size_t levels) const override {
        return side == Side::Buy ? topLevels(bids_, levels) : topLevels(asks_, levels);
    }

    void reserveOrders(std::size_t expectedOrders) override {
        pool_.reserve(expectedOrders);
        index_.reserve(expectedOrders);
    }

protected:
    void match(Order& incoming, std::vector<Trade>& out) override {
        if (incoming.side == Side::Buy) matchAgainst(incoming, asks_, out);
        else                            matchAgainst(incoming, bids_, out);
    }

    void rest(const Order& order) override {
        if (order.side == Side::Buy) restIn(order, bids_);
        else                         restIn(order, asks_);
    }

private:
    typename LevelSet::Bids bids_;
    typename LevelSet::Asks asks_;
    Pool  pool_;
    Index index_;

    // Take node n out of its level's list by joining its two neighbours to each other.
    void unlink(ListLevel& level, std::uint32_t n) {
        std::uint32_t prev = pool_[n].prev, next = pool_[n].next;
        if (prev != kNoNode) pool_[prev].next = next; else level.head = next;
        if (next != kNoNode) pool_[next].prev = prev; else level.tail = prev;
        --level.count;
    }

    // Unlink node n, forget its id and give the node back to the pool.
    void destroy(ListLevel& level, std::uint32_t n) {
        unlink(level, n);
        index_.erase(pool_[n].id);
        pool_.release(n);
    }

    template <typename Book>
    void matchAgainst(Order& incoming, Book& opposite, std::vector<Trade>& out) {
        while (incoming.quantity > 0 && !opposite.empty()) {
            Price price = opposite.bestPrice();            // best price level on the other side
            if (!crosses(incoming, price)) break;          // prices don't overlap -> stop

            ListLevel& level = opposite.best();
            while (incoming.quantity > 0 && level.head != kNoNode) {
                std::uint32_t n       = level.head;        // oldest order at this price goes first
                auto&         resting = pool_[n];
                Quantity      qty     = std::min(incoming.quantity, resting.quantity);

                out.push_back(makeTrade(incoming, resting.id, price, qty));

                incoming.quantity -= qty;
                resting.quantity  -= qty;                  // partial fill if this stays > 0

                if (resting.quantity == 0) destroy(level, n);
            }
            if (level.head == kNoNode) opposite.levelEmptied(price);
        }
    }

    template <typename Book>
    void restIn(const Order& order, Book& own) {
        std::uint32_t n = pool_.allocate();                // may move the array, so take references after it
        auto& node = pool_[n];
        node.id       = order.id;
        node.side     = order.side;
        node.price    = order.price;
        node.quantity = order.quantity;
        pool_.timestamp(n) = order.timestamp;

        ListLevel& level = own.get(order.price);           // creates the level if it doesn't exist
        node.prev = level.tail;                            // join the back of the queue
        node.next = kNoNode;
        if (level.tail != kNoNode) pool_[level.tail].next = n; else level.head = n;
        level.tail = n;
        ++level.count;

        index_.insert(order.id, n);
    }

    template <typename Book>
    void removeFrom(Book& book, std::uint32_t n) {
        Price      price = pool_[n].price;
        ListLevel& level = book.find(price);
        destroy(level, n);
        if (level.count == 0) book.levelEmptied(price);
    }

    template <typename Book>
    std::vector<DepthLevel> topLevels(const Book& book, std::size_t levels) const {
        std::vector<DepthLevel> out;
        book.forEach([&](Price price, const ListLevel& level) {
            if (out.size() == levels) return false;
            Quantity total = 0;
            for (std::uint32_t n = level.head; n != kNoNode; n = pool_[n].next) total += pool_[n].quantity;
            out.push_back({price, total, level.count});
            return true;
        });
        return out;
    }
};

using IntrusiveBookC2 = IntrusiveBook<StdIndex,  FatPool,  MapLevelSet>;   // pool + intrusive list
using IntrusiveBookC3 = IntrusiveBook<OpenIndex, FatPool,  MapLevelSet>;   // + open-addressing hash map
using CustomBook      = IntrusiveBook<OpenIndex, SlimPool, MapLevelSet>;   // + cache-friendly layout = the full engine C
using HybridBook      = IntrusiveBook<OpenIndex, SlimPool, FlatLevelSet>;  // engine D: C with B's flat price array
