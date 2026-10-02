#include "OrderBook.hpp"
#include <cassert>
#include <cstdio>

int main() {
    // 1. Non-crossing limit orders just rest
    {
        OrderBook b;
        assert(b.addOrder({1, Side::Buy,  OrderType::Limit, 10000, 10}).empty());
        assert(b.addOrder({2, Side::Sell, OrderType::Limit, 10100, 10}).empty());
        assert(b.hasOrder(1) && b.hasOrder(2));
    }
    // 2. Partial fill: resting order keeps the rest
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 20});
        auto t = b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 5});
        assert(t.size() == 1 && t[0].quantity == 5 && t[0].price == 10000);
        assert(b.quantityAt(Side::Sell, 10000) == 15);
        assert(!b.hasOrder(2));
    }
    // 3. Time priority: older order at the same price fills first
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({3, Side::Buy, OrderType::Limit, 10000, 5});
        assert(t.size() == 1 && t[0].sellId == 1);
        assert(!b.hasOrder(1) && b.hasOrder(2));
    }
    // 4. Trade happens at the resting price, not the incoming price
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({2, Side::Buy, OrderType::Limit, 10500, 5});
        assert(t.size() == 1 && t[0].price == 10000);
    }
    // 5. Market order sweeps levels; leftover is dropped, not rested
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 9900, 5});
        auto t = b.addOrder({3, Side::Sell, OrderType::Market, 0, 20});
        assert(t.size() == 2 && t[0].price == 10000 && t[1].price == 9900);
        assert(!b.hasOrder(3));
    }
    // 6. Limit order fills what it can, rests the remainder
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 8});
        assert(b.quantityAt(Side::Buy, 10000) == 3 && b.hasOrder(2));
    }
    // 7. Cancel removes the order; cancelling twice fails
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        assert(b.cancelOrder(1));
        assert(!b.cancelOrder(1));
        assert(b.quantityAt(Side::Buy, 10000) == 0);
    }
    // 8. Cancelled order can't be matched
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.cancelOrder(1);
        assert(b.addOrder({2, Side::Buy, OrderType::Market, 0, 5}).empty());
    }

    // ---- Phase 2: price-time priority ----

    // 9. Price priority (asks): the cheaper ask fills first, even though it arrived later
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10100, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({3, Side::Buy, OrderType::Market, 0, 5});
        assert(t.size() == 1 && t[0].sellId == 2 && t[0].price == 10000);
        assert(b.hasOrder(1) && !b.hasOrder(2));
    }
    // 10. Price priority (bids): the higher bid fills first, even though it arrived later
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 9900, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({3, Side::Sell, OrderType::Market, 0, 5});
        assert(t.size() == 1 && t[0].buyId == 2 && t[0].price == 10000);
        assert(b.hasOrder(1) && !b.hasOrder(2));
    }
    // 11. Price first, then time: sweep goes best price -> oldest at that price -> next price
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10100, 5});   // oldest, but worst price
        b.addOrder({2, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({3, Side::Sell, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({4, Side::Buy, OrderType::Limit, 10100, 12});
        assert(t.size() == 3);
        assert(t[0].sellId == 2 && t[0].quantity == 5 && t[0].price == 10000);
        assert(t[1].sellId == 3 && t[1].quantity == 5 && t[1].price == 10000);
        assert(t[2].sellId == 1 && t[2].quantity == 2 && t[2].price == 10100);
        assert(b.quantityAt(Side::Sell, 10100) == 3);
    }
    // 12. A partially filled order keeps its place at the front of the queue
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 10});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10000, 10});
        b.addOrder({3, Side::Buy, OrderType::Limit, 10000, 4});    // #1 now has 6 left
        auto t = b.addOrder({4, Side::Buy, OrderType::Limit, 10000, 8});
        assert(t.size() == 2);
        assert(t[0].sellId == 1 && t[0].quantity == 6);            // #1 finishes first
        assert(t[1].sellId == 2 && t[1].quantity == 2);
        assert(!b.hasOrder(1) && b.quantityAt(Side::Sell, 10000) == 8);
    }

    // ---- Phase 2: best bid / best ask / spread ----

    // 13. Empty book has no best prices and no spread
    {
        OrderBook b;
        assert(!b.bestBid() && !b.bestAsk() && !b.spread());
    }
    // 14. Best bid is the highest bid, best ask is the lowest ask, spread is the gap
    {
        OrderBook b;
        b.addOrder({1, Side::Buy,  OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy,  OrderType::Limit, 10050, 5});
        assert(b.bestBid() == 10050);
        assert(!b.bestAsk() && !b.spread());                       // one-sided book: no spread
        b.addOrder({3, Side::Sell, OrderType::Limit, 10200, 5});
        b.addOrder({4, Side::Sell, OrderType::Limit, 10120, 5});
        assert(b.bestAsk() == 10120);
        assert(b.spread() == 70);
    }
    // 15. Best prices move when the top level is cancelled or filled
    {
        OrderBook b;
        b.addOrder({1, Side::Buy,  OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy,  OrderType::Limit, 10050, 5});
        b.addOrder({3, Side::Sell, OrderType::Limit, 10100, 5});
        b.addOrder({4, Side::Sell, OrderType::Limit, 10200, 5});
        b.cancelOrder(2);
        assert(b.bestBid() == 10000);
        b.addOrder({5, Side::Buy, OrderType::Market, 0, 5});       // fills all of #3
        assert(b.bestAsk() == 10200);
        assert(b.spread() == 200);
    }

    // ---- Phase 2: market depth ----

    // 16. Depth returns the top N levels, best price first, with orders at a price added together
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 10});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 7});    // same level as #1
        b.addOrder({3, Side::Buy, OrderType::Limit, 9900, 5});
        b.addOrder({4, Side::Buy, OrderType::Limit, 10100, 3});
        b.addOrder({5, Side::Sell, OrderType::Limit, 10300, 4});
        b.addOrder({6, Side::Sell, OrderType::Limit, 10200, 6});

        auto bids = b.depth(Side::Buy, 2);
        assert(bids.size() == 2);
        assert(bids[0].price == 10100 && bids[0].quantity == 3  && bids[0].orderCount == 1);
        assert(bids[1].price == 10000 && bids[1].quantity == 17 && bids[1].orderCount == 2);

        auto asks = b.depth(Side::Sell, 2);
        assert(asks.size() == 2);
        assert(asks[0].price == 10200 && asks[0].quantity == 6);
        assert(asks[1].price == 10300 && asks[1].quantity == 4);
    }
    // 17. Asking for more levels than exist returns what there is; empty side returns nothing
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 10});
        assert(b.depth(Side::Buy, 5).size() == 1);
        assert(b.depth(Side::Buy, 0).empty());
        assert(b.depth(Side::Sell, 5).empty());
    }
    // 18. Depth reflects partial fills
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 10});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 4});
        auto asks = b.depth(Side::Sell, 1);
        assert(asks.size() == 1 && asks[0].quantity == 6 && asks[0].orderCount == 1);
    }

    // ---- Phase 2: rejecting bad orders ----

    // 19. Zero-quantity orders are rejected (limit and market)
    {
        OrderBook b;
        Order zeroLimit{1, Side::Buy, OrderType::Limit, 10000, 0};
        assert(b.validate(zeroLimit) == RejectReason::ZeroQuantity);
        assert(b.addOrder(zeroLimit).empty());
        assert(!b.hasOrder(1) && !b.bestBid());

        b.addOrder({2, Side::Sell, OrderType::Limit, 10000, 5});
        assert(b.addOrder({3, Side::Buy, OrderType::Market, 0, 0}).empty());
        assert(b.quantityAt(Side::Sell, 10000) == 5 && b.tradeHistory().empty());
    }
    // 20. An id that is already resting is rejected; the original order is untouched
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        Order dup{1, Side::Buy, OrderType::Limit, 10100, 9};
        assert(b.validate(dup) == RejectReason::DuplicateId);
        assert(b.addOrder(dup).empty());
        assert(b.quantityAt(Side::Buy, 10000) == 5 && b.quantityAt(Side::Buy, 10100) == 0);
    }
    // 21. A duplicate id must not trade, even if its price would cross
    {
        OrderBook b;
        b.addOrder({1, Side::Buy,  OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10100, 5});
        assert(b.addOrder({1, Side::Buy, OrderType::Limit, 10100, 5}).empty());
        assert(b.hasOrder(2) && b.tradeHistory().empty());
    }
    // 22. A normal order validates as None
    {
        OrderBook b;
        assert(b.validate({1, Side::Buy, OrderType::Limit, 10000, 5}) == RejectReason::None);
    }

    // ---- Phase 2: order timestamps ----

    // 23. getOrder returns the resting order, or nothing if the id isn't in the book
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        auto o = b.getOrder(1);
        assert(o && o->id == 1 && o->price == 10000 && o->quantity == 5);
        assert(!b.getOrder(99));
    }
    // 24. The book stamps each order on arrival; later orders never have earlier timestamps.
    //     (<= not <: two orders can land inside the same clock tick.)
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 5});
        b.addOrder({3, Side::Buy, OrderType::Limit, 9900, 5});
        auto t1 = b.getOrder(1)->timestamp, t2 = b.getOrder(2)->timestamp, t3 = b.getOrder(3)->timestamp;
        assert(t1 > 0);
        assert(t1 <= t2 && t2 <= t3);
    }
    // 25. A timestamp supplied by the caller is ignored, and a partial fill doesn't change it
    {
        OrderBook b;
        Order o{1, Side::Sell, OrderType::Limit, 10000, 10};
        o.timestamp = 1;
        b.addOrder(o);
        Timestamp stamped = b.getOrder(1)->timestamp;
        assert(stamped != 1);
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 4});
        assert(b.getOrder(1)->quantity == 6 && b.getOrder(1)->timestamp == stamped);
    }

    // ---- Phase 2: trade history with timestamps ----

    // 26. History keeps every trade from every order, in the order they happened
    {
        OrderBook b;
        assert(b.tradeHistory().empty());
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10100, 5});
        b.addOrder({3, Side::Buy, OrderType::Limit, 10000, 3});    // 1 trade
        b.addOrder({4, Side::Buy, OrderType::Market, 0, 6});       // 2 trades
        const auto& h = b.tradeHistory();
        assert(h.size() == 3);
        assert(h[0].buyId == 3 && h[0].sellId == 1 && h[0].quantity == 3 && h[0].price == 10000);
        assert(h[1].buyId == 4 && h[1].sellId == 1 && h[1].quantity == 2 && h[1].price == 10000);
        assert(h[2].buyId == 4 && h[2].sellId == 2 && h[2].quantity == 4 && h[2].price == 10100);
    }
    // 27. Trades are timestamped: never before the resting order arrived, never going backwards,
    //     and all trades caused by one incoming order share its arrival time
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10100, 5});
        Timestamp resting1 = b.getOrder(1)->timestamp;
        b.addOrder({3, Side::Buy, OrderType::Limit, 10000, 3});
        auto fills = b.addOrder({4, Side::Buy, OrderType::Market, 0, 6});
        const auto& h = b.tradeHistory();
        assert(h[0].timestamp > 0 && h[0].timestamp >= resting1);
        assert(h[0].timestamp <= h[1].timestamp && h[1].timestamp <= h[2].timestamp);
        assert(h[1].timestamp == h[2].timestamp);
        assert(fills.size() == 2 && fills[0].timestamp == h[1].timestamp);   // returned trades match history
    }

    // ---- Phase 2: order modify (cancel + replace) ----

    // 28. Modify moves the order to its new price and quantity
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        assert(b.modifyOrder(1, 10050, 8));
        assert(b.quantityAt(Side::Buy, 10000) == 0 && b.quantityAt(Side::Buy, 10050) == 8);
        assert(b.bestBid() == 10050 && b.getOrder(1)->side == Side::Buy);
    }
    // 29. Modify loses time priority, even if only the quantity changed
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10000, 5});
        Timestamp before = b.getOrder(1)->timestamp;
        assert(b.modifyOrder(1, 10000, 4));                        // #1 goes behind #2
        assert(b.getOrder(1)->timestamp >= before);
        auto t = b.addOrder({3, Side::Buy, OrderType::Limit, 10000, 5});
        assert(t.size() == 1 && t[0].sellId == 2);
        assert(b.hasOrder(1) && !b.hasOrder(2));
    }
    // 30. Modifying to a price that crosses the spread trades immediately
    {
        OrderBook b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10100, 5});
        b.addOrder({2, Side::Buy,  OrderType::Limit, 10000, 8});
        assert(b.modifyOrder(2, 10100, 8));
        const auto& h = b.tradeHistory();
        assert(h.size() == 1 && h[0].buyId == 2 && h[0].sellId == 1 && h[0].quantity == 5);
        assert(!b.hasOrder(1) && b.getOrder(2)->quantity == 3);    // leftover rests at the new price
        assert(b.bestBid() == 10100);
    }
    // 31. Modify fails, and changes nothing, for an unknown id or a zero quantity
    {
        OrderBook b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        Timestamp before = b.getOrder(1)->timestamp;
        assert(!b.modifyOrder(99, 10000, 5));
        assert(!b.modifyOrder(1, 10050, 0));
        assert(b.getOrder(1)->price == 10000 && b.getOrder(1)->quantity == 5);
        assert(b.getOrder(1)->timestamp == before);
    }

    std::printf("All 31 tests passed.\n");
}
