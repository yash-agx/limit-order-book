#pragma once
#include "OrderBookBase.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <memory_resource>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Implementation C - custom, low-level data structures. Built up in steps so each
// optimisation can be measured on its own (the "ablation"):
//
//   C2 = pool + intrusive list
//   C3 = C2 + open-addressing hash map for the id index
//   C4 = C3 + cache-friendly node layout   (this is the complete engine C)
//   D  = C4 with engine B's flat price-level array in place of the std::map (over a window
//        of prices, with far-away levels kept in a small side map - see FlatLevels)
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
// by price - but only over a *window* of prices around where trading actually happens.
// levels_[price - base_] is the level for that price, so reaching a level is one
// subtraction instead of a walk down a tree. The best price is remembered, and when the
// best level empties we step along the array to the next level that has orders.
//
// Why a window, instead of an array stretched to cover every price ever seen (which is what
// this class did at first)? Real exchange data contains a handful of orders priced absurdly
// far from the market: pre-open stub quotes at $0.01 and $199,999.99. An array that has to
// reach $199,999.99 one cent at a time needs ~20 million slots (229 MiB), and worse, the
// first later order priced *below* the array's base has to insert at the front, which
// shifts all 20 million slots - a single 45 ms stall, measured on real AAPL and AMZN data
// (see the README section "Fixing D: a price window plus a side map").
//
// So the array covers at most kMaxWindow consecutive prices. Any price outside it goes into
// `far_`, a small std::map of price -> level, sorted the same way. Everything below treats
// the two together as one sorted sequence of levels: the best price is the better of the two
// bests, and forEach walks them in step. Three consequences:
//   - a far-away order costs a std::map insert instead of 20 million element moves;
//   - if the market ever reaches a far level it matches correctly straight out of the map;
//   - if far levels pile up (the market has moved somewhere else entirely) the window is
//     rebuilt around the market and everything that now fits is pulled in.
// Growing and rebuilding both allocate a new array and copy the occupied levels into it, so
// nothing is ever inserted at the front and the cost is bounded by kMaxWindow.
//
// Invariant: no price in `far_` lies inside the current window. That is what lets find()
// and get() decide where a level lives from the price alone.
//
// Note on references: get() may rebuild the array, which invalidates references to levels
// handed out earlier. The engine only ever holds one level reference at a time and finishes
// with it before calling get() again, so this is safe.
// ---------------------------------------------------------------------------------------
template <bool IsBidSide>
class FlatLevels {
public:
    bool empty() const { return occupiedNear_ == 0 && far_.empty(); }

    Price bestPrice() const {
        if (far_.empty())       return bestNear_;             // the usual case: no far levels at all
        if (occupiedNear_ == 0) return far_.begin()->first;
        return isBetter(bestNear_, far_.begin()->first) ? bestNear_ : far_.begin()->first;
    }

    ListLevel& best() {
        Price price = bestPrice();
        return inWindow(price) ? at(price) : far_.begin()->second;
    }

    // The level a new order is about to join (so an empty level counts as occupied from now).
    ListLevel& get(Price price) {
        if (inWindow(price)) return getNear(price);            // the common case: one bounds check
        return getOutside(price);
    }

    ListLevel& find(Price price) {                             // the level must exist
        return inWindow(price) ? at(price) : far_.find(price)->second;
    }

    void levelEmptied(Price price) {
        if (!inWindow(price)) { far_.erase(price); return; }
        --occupiedNear_;
        if (occupiedNear_ > 0 && price == bestNear_)
            while (at(bestNear_).count == 0) bestNear_ += kAway;   // some level still has orders, so this stops
    }

    // Call visit(price, level) for each level, best price first, until it returns false.
    // The window and the side map are two sorted sequences, so this walks them together and
    // always takes the better of the two next prices (a merge).
    template <typename Visit>
    void forEach(Visit visit) const {
        auto        it   = far_.begin();
        std::size_t left = occupiedNear_;
        for (Price price = bestNear_; left > 0; price += kAway) {
            const ListLevel& level = at(price);
            if (level.count == 0) continue;
            --left;
            for (; it != far_.end() && isBetter(it->first, price); ++it)   // far levels that come first
                if (!visit(it->first, it->second)) return;
            if (!visit(price, level)) return;
        }
        for (; it != far_.end(); ++it)                                     // whatever is left in the map
            if (!visit(it->first, it->second)) return;
    }

private:
    static constexpr Price kAway = IsBidSide ? -1 : 1;   // the direction that leads away from the other side

    static constexpr std::size_t kMinWindow = 2048;      // the window starts this wide ...
    static constexpr std::size_t kMaxWindow = 65536;     // ... and never grows past this ($655 of prices)
    static constexpr std::size_t kFarLimit  = 64;        // this many far levels triggers a recentre

    // The map is sorted best price first, exactly like MapLevels above.
    using FarCompare = std::conditional_t<IsBidSide, std::greater<Price>, std::less<Price>>;

    static bool isBetter(Price a, Price b) { return IsBidSide ? a > b : a < b; }

    std::vector<ListLevel>                 levels_;          // the window: base_ .. base_ + size - 1
    Price                                  base_ = 0;
    Price                                  bestNear_ = 0;    // only meaningful while occupiedNear_ > 0
    std::size_t                            occupiedNear_ = 0;  // window levels with at least one order
    std::map<Price, ListLevel, FarCompare> far_;             // levels too far away for the window
    std::size_t                            farLimit_ = kFarLimit;

    bool inWindow(Price price) const {
        return !levels_.empty() && price >= base_ &&
               price - base_ < static_cast<Price>(levels_.size());
    }
    ListLevel&       at(Price price)       { return levels_[static_cast<std::size_t>(price - base_)]; }
    const ListLevel& at(Price price) const { return levels_[static_cast<std::size_t>(price - base_)]; }

    ListLevel& getNear(Price price) {
        ListLevel& level = at(price);
        if (level.count == 0) {
            if (occupiedNear_ == 0 || isBetter(price, bestNear_)) bestNear_ = price;
            ++occupiedNear_;
        }
        return level;
    }

    // A price outside the window: either the window can be moved or widened to include it,
    // or the level belongs in the side map.
    ListLevel& getOutside(Price price) {
        auto found = far_.find(price);
        if (found != far_.end()) return found->second;         // a far level that already exists

        if (fitWindow(price)) return getNear(price);           // grown or re-placed to cover it

        if (far_.size() + 1 > farLimit_) {                     // the market may have moved out here
            recentre(price);
            if (inWindow(price)) return getNear(price);
        }
        return far_[price];     // a brand-new far level, count 0; its order is linked straight after
    }

    // Try to make `price` addressable in the window. Returns false if it is too far away,
    // i.e. no window of kMaxWindow prices could hold both it and the levels already resting.
    bool fitWindow(Price price) {
        std::size_t size = levels_.empty() ? kMinWindow : levels_.size();

        if (occupiedNear_ == 0) {                              // nothing to preserve: put the window here
            rebuild(price - static_cast<Price>(size / 2), size);
            return true;
        }
        // Cheap reject before scanning the window: every resting level sits inside it, so a
        // price a whole kMaxWindow beyond either edge can never share a window with them.
        Price highEdge = base_ + static_cast<Price>(levels_.size()) - 1;
        if (price < base_    && base_ - price    >= static_cast<Price>(kMaxWindow)) return false;
        if (price > highEdge && price - highEdge >= static_cast<Price>(kMaxWindow)) return false;

        std::pair<Price, Price> occupied = occupiedRange();
        Price       low  = std::min(price, occupied.first);
        Price       high = std::max(price, occupied.second);
        std::size_t need = static_cast<std::size_t>(high - low) + 1;
        if (need > kMaxWindow) return false;

        while (size < need) size *= 2;                         // doubles at most 5 times, kMinWindow -> kMaxWindow
        rebuild(low - static_cast<Price>((size - need) / 2), size);   // centre the span in the new array
        return true;
    }

    // Enough far levels have piled up that the window is probably in the wrong place.
    // Rebuild it at full width around the best price: the market is wherever the best price
    // is, and far-away junk is by definition on the *worse* side of it.
    void recentre(Price price) {
        Price anchor = empty() ? price : bestPrice();
        rebuild(anchor - static_cast<Price>(kMaxWindow / 2), kMaxWindow);
        // If the junk is still out there, don't try again on the very next far order: a
        // recentre that didn't help must not happen once per order.
        farLimit_ = std::max(kFarLimit, far_.size() * 2);
    }

    // The lowest and highest prices in the window that have orders.
    std::pair<Price, Price> occupiedRange() const {
        Price       low = 0, high = 0;
        std::size_t seen = 0;
        for (std::size_t i = 0; i < levels_.size() && seen < occupiedNear_; ++i) {
            if (levels_[i].count == 0) continue;
            Price price = base_ + static_cast<Price>(i);
            if (seen++ == 0) low = price;
            high = price;
        }
        return {low, high};
    }

    // Move the window to [newBase, newBase + size). Occupied levels that fit are copied into
    // the new array; any that don't (only possible on a recentre) go to the side map, and far
    // levels that now fit come in. One allocation and one pass - never a front-insert.
    void rebuild(Price newBase, std::size_t size) {
        std::vector<ListLevel>                   fresh(size);
        std::vector<std::pair<Price, ListLevel>> evicted;
        std::size_t occupied = 0;
        Price       best     = 0;

        auto place = [&](Price price, const ListLevel& level) {
            if (price < newBase || price - newBase >= static_cast<Price>(size)) {
                evicted.push_back({price, level});
                return;
            }
            fresh[static_cast<std::size_t>(price - newBase)] = level;
            if (occupied == 0 || isBetter(price, best)) best = price;
            ++occupied;
        };

        std::size_t seen = 0;
        for (std::size_t i = 0; i < levels_.size() && seen < occupiedNear_; ++i)
            if (levels_[i].count != 0) { ++seen; place(base_ + static_cast<Price>(i), levels_[i]); }

        for (auto it = far_.begin(); it != far_.end(); ) {
            if (it->first >= newBase && it->first - newBase < static_cast<Price>(size)) {
                place(it->first, it->second);
                it = far_.erase(it);
            } else {
                ++it;
            }
        }

        levels_.swap(fresh);
        base_         = newBase;
        occupiedNear_ = occupied;
        bestNear_     = best;
        for (const std::pair<Price, ListLevel>& level : evicted) far_[level.first] = level.second;
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
