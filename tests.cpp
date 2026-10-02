#include "Engines.hpp"
#include "FlatBook.hpp"
#include "OrderBook.hpp"
#include "OrderFlow.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>

// Every test below runs once for each engine: `Book` is the engine's class.
template <typename Book>
void engineTests() {
    // 1. Non-crossing limit orders just rest
    {
        Book b;
        assert(b.addOrder({1, Side::Buy,  OrderType::Limit, 10000, 10}).empty());
        assert(b.addOrder({2, Side::Sell, OrderType::Limit, 10100, 10}).empty());
        assert(b.hasOrder(1) && b.hasOrder(2));
    }
    // 2. Partial fill: resting order keeps the rest
    {
        Book b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 20});
        auto t = b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 5});
        assert(t.size() == 1 && t[0].quantity == 5 && t[0].price == 10000);
        assert(b.quantityAt(Side::Sell, 10000) == 15);
        assert(!b.hasOrder(2));
    }
    // 3. Time priority: older order at the same price fills first
    {
        Book b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({3, Side::Buy, OrderType::Limit, 10000, 5});
        assert(t.size() == 1 && t[0].sellId == 1);
        assert(!b.hasOrder(1) && b.hasOrder(2));
    }
    // 4. Trade happens at the resting price, not the incoming price
    {
        Book b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({2, Side::Buy, OrderType::Limit, 10500, 5});
        assert(t.size() == 1 && t[0].price == 10000);
    }
    // 5. Market order sweeps levels; leftover is dropped, not rested
    {
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 9900, 5});
        auto t = b.addOrder({3, Side::Sell, OrderType::Market, 0, 20});
        assert(t.size() == 2 && t[0].price == 10000 && t[1].price == 9900);
        assert(!b.hasOrder(3));
    }
    // 6. Limit order fills what it can, rests the remainder
    {
        Book b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 8});
        assert(b.quantityAt(Side::Buy, 10000) == 3 && b.hasOrder(2));
    }
    // 7. Cancel removes the order; cancelling twice fails
    {
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        assert(b.cancelOrder(1));
        assert(!b.cancelOrder(1));
        assert(b.quantityAt(Side::Buy, 10000) == 0);
    }
    // 8. Cancelled order can't be matched
    {
        Book b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.cancelOrder(1);
        assert(b.addOrder({2, Side::Buy, OrderType::Market, 0, 5}).empty());
    }

    // ---- Phase 2: price-time priority ----

    // 9. Price priority (asks): the cheaper ask fills first, even though it arrived later
    {
        Book b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10100, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({3, Side::Buy, OrderType::Market, 0, 5});
        assert(t.size() == 1 && t[0].sellId == 2 && t[0].price == 10000);
        assert(b.hasOrder(1) && !b.hasOrder(2));
    }
    // 10. Price priority (bids): the higher bid fills first, even though it arrived later
    {
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 9900, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 5});
        auto t = b.addOrder({3, Side::Sell, OrderType::Market, 0, 5});
        assert(t.size() == 1 && t[0].buyId == 2 && t[0].price == 10000);
        assert(b.hasOrder(1) && !b.hasOrder(2));
    }
    // 11. Price first, then time: sweep goes best price -> oldest at that price -> next price
    {
        Book b;
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
        Book b;
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
        Book b;
        assert(!b.bestBid() && !b.bestAsk() && !b.spread());
    }
    // 14. Best bid is the highest bid, best ask is the lowest ask, spread is the gap
    {
        Book b;
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
        Book b;
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
        Book b;
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
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 10});
        assert(b.depth(Side::Buy, 5).size() == 1);
        assert(b.depth(Side::Buy, 0).empty());
        assert(b.depth(Side::Sell, 5).empty());
    }
    // 18. Depth reflects partial fills
    {
        Book b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 10});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 4});
        auto asks = b.depth(Side::Sell, 1);
        assert(asks.size() == 1 && asks[0].quantity == 6 && asks[0].orderCount == 1);
    }

    // ---- Phase 2: rejecting bad orders ----

    // 19. Zero-quantity orders are rejected (limit and market)
    {
        Book b;
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
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        Order dup{1, Side::Buy, OrderType::Limit, 10100, 9};
        assert(b.validate(dup) == RejectReason::DuplicateId);
        assert(b.addOrder(dup).empty());
        assert(b.quantityAt(Side::Buy, 10000) == 5 && b.quantityAt(Side::Buy, 10100) == 0);
    }
    // 21. A duplicate id must not trade, even if its price would cross
    {
        Book b;
        b.addOrder({1, Side::Buy,  OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Sell, OrderType::Limit, 10100, 5});
        assert(b.addOrder({1, Side::Buy, OrderType::Limit, 10100, 5}).empty());
        assert(b.hasOrder(2) && b.tradeHistory().empty());
    }
    // 22. A normal order validates as None
    {
        Book b;
        assert(b.validate({1, Side::Buy, OrderType::Limit, 10000, 5}) == RejectReason::None);
    }

    // ---- Phase 2: order timestamps ----

    // 23. getOrder returns the resting order, or nothing if the id isn't in the book
    {
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        auto o = b.getOrder(1);
        assert(o && o->id == 1 && o->price == 10000 && o->quantity == 5);
        assert(!b.getOrder(99));
    }
    // 24. The book stamps each order on arrival; later orders never have earlier timestamps.
    //     (<= not <: two orders can land inside the same clock tick.)
    {
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 5});
        b.addOrder({3, Side::Buy, OrderType::Limit, 9900, 5});
        auto t1 = b.getOrder(1)->timestamp, t2 = b.getOrder(2)->timestamp, t3 = b.getOrder(3)->timestamp;
        assert(t1 > 0);
        assert(t1 <= t2 && t2 <= t3);
    }
    // 25. A timestamp supplied by the caller is ignored, and a partial fill doesn't change it
    {
        Book b;
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
        Book b;
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
        Book b;
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
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        assert(b.modifyOrder(1, 10050, 8));
        assert(b.quantityAt(Side::Buy, 10000) == 0 && b.quantityAt(Side::Buy, 10050) == 8);
        assert(b.bestBid() == 10050 && b.getOrder(1)->side == Side::Buy);
    }
    // 29. Modify loses time priority, even if only the quantity changed
    {
        Book b;
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
        Book b;
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
        Book b;
        b.addOrder({1, Side::Buy, OrderType::Limit, 10000, 5});
        Timestamp before = b.getOrder(1)->timestamp;
        assert(!b.modifyOrder(99, 10000, 5));
        assert(!b.modifyOrder(1, 10050, 0));
        assert(b.getOrder(1)->price == 10000 && b.getOrder(1)->quantity == 5);
        assert(b.getOrder(1)->timestamp == before);
    }

    // ---- Phase 3: order-flow generator ----

    auto sameAction = [](const Action& x, const Action& y) {
        return x.type == y.type && x.order.id == y.order.id && x.order.side == y.order.side &&
               x.order.type == y.order.type && x.order.price == y.order.price &&
               x.order.quantity == y.order.quantity;
    };

    // 32. Same seed -> exactly the same order flow; a different seed -> a different one
    {
        FlowConfig cfg;
        cfg.targetBookSize = 200;
        OrderFlowGenerator g1(cfg), g2(cfg);
        auto p1 = g1.prefill(), p2 = g2.prefill();
        auto f1 = g1.generate(5000), f2 = g2.generate(5000);
        assert(p1.size() == p2.size() && f1.size() == 5000 && f2.size() == 5000);
        for (std::size_t i = 0; i < p1.size(); ++i) assert(sameAction(p1[i], p2[i]));
        for (std::size_t i = 0; i < f1.size(); ++i) assert(sameAction(f1[i], f2[i]));

        cfg.seed = 7;
        OrderFlowGenerator g3(cfg);
        g3.prefill();
        auto f3 = g3.generate(5000);
        std::size_t different = 0;
        for (std::size_t i = 0; i < f1.size(); ++i) different += !sameAction(f1[i], f3[i]);
        assert(different > 0);
    }
    // 33. Prefill puts exactly targetBookSize orders in the book and causes no trades
    {
        FlowConfig cfg;
        cfg.targetBookSize = 500;
        OrderFlowGenerator gen(cfg);
        auto prefill = gen.prefill();
        assert(prefill.size() == 500 && gen.bookSize() == 500);

        Book b;
        for (const Action& a : prefill) {
            assert(a.type == ActionType::Add && a.order.type == OrderType::Limit);
            apply(b, a);
        }
        assert(b.tradeHistory().empty());
        assert(b.bestBid() < cfg.midPrice && b.bestAsk() > cfg.midPrice);
    }
    // 34. The flow is valid: every order is well-formed, every cancel/modify targets a resting
    //     order, the mix contains every kind of action, and the book stays near its target size
    {
        FlowConfig cfg;
        cfg.targetBookSize = 1000;
        OrderFlowGenerator gen(cfg);
        auto prefill = gen.prefill();
        auto flow    = gen.generate(20000);

        Book b;
        for (const Action& a : prefill) apply(b, a);

        std::size_t limits = 0, markets = 0, cancels = 0, modifies = 0;
        for (const Action& a : flow) {
            if (a.type == ActionType::Add) {
                assert(b.validate(a.order) == RejectReason::None);
                if (a.order.type == OrderType::Limit) { assert(a.order.price > 0); ++limits; }
                else                                  ++markets;
            } else {
                assert(b.hasOrder(a.order.id));
                if (a.type == ActionType::Cancel) ++cancels; else ++modifies;
            }
            apply(b, a);
        }
        assert(limits > 0 && markets > 0 && cancels > 0 && modifies > 0);
        assert(!b.tradeHistory().empty());
        assert(*b.bestBid() < *b.bestAsk());                       // the book is never crossed

        // The generator's idea of the book matches a real replay of its actions
        std::size_t resting = 0;
        for (const DepthLevel& lvl : b.depth(Side::Buy, 100000))  resting += lvl.orderCount;
        for (const DepthLevel& lvl : b.depth(Side::Sell, 100000)) resting += lvl.orderCount;
        assert(resting == gen.bookSize());
        assert(resting > 900 && resting < 1100);
    }

    // ---- Phase 4: cases that stress the data structures ----

    // 35. One very long queue with holes: cancel every second order, the rest still fill oldest-first
    {
        Book b;
        for (OrderId id = 1; id <= 2000; ++id) b.addOrder({id, Side::Sell, OrderType::Limit, 10000, 1});
        for (OrderId id = 2; id <= 2000; id += 2) assert(b.cancelOrder(id));
        assert(b.quantityAt(Side::Sell, 10000) == 1000 && b.depth(Side::Sell, 1)[0].orderCount == 1000);

        auto t = b.addOrder({5000, Side::Buy, OrderType::Limit, 10000, 600});
        assert(t.size() == 600);
        for (std::size_t i = 0; i < t.size(); ++i) assert(t[i].sellId == 2 * i + 1);   // 1, 3, 5, ...
        assert(!b.hasOrder(1199) && b.hasOrder(1201) && !b.hasOrder(1200));

        b.addOrder({5001, Side::Sell, OrderType::Limit, 10000, 1});                    // joins the back
        auto rest = b.addOrder({5002, Side::Buy, OrderType::Market, 0, 1000});
        assert(rest.size() == 401 && rest.front().sellId == 1201 && rest.back().sellId == 5001);
        assert(!b.bestAsk());
    }
    // 36. Prices far apart, and far from where the book started
    {
        Book b;
        b.addOrder({1, Side::Buy,  OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy,  OrderType::Limit, 1, 5});
        b.addOrder({3, Side::Sell, OrderType::Limit, 150000, 5});
        b.addOrder({4, Side::Sell, OrderType::Limit, 10100, 5});
        assert(b.bestBid() == 10000 && b.bestAsk() == 10100);
        assert(b.depth(Side::Buy, 5).size() == 2 && b.depth(Side::Sell, 5).size() == 2);
        assert(b.cancelOrder(1) && b.bestBid() == 1);
        auto t = b.addOrder({5, Side::Buy, OrderType::Market, 0, 8});
        assert(t.size() == 2 && t[0].price == 10100 && t[1].price == 150000);
        assert(b.getOrder(3)->quantity == 2);
    }
    // 37. Very large order ids work like any other id
    {
        Book b;
        const OrderId big = 1ull << 40;
        b.addOrder({big, Side::Buy, OrderType::Limit, 10000, 5});
        b.addOrder({big + 1, Side::Buy, OrderType::Limit, 10000, 5});
        assert(b.hasOrder(big) && b.getOrder(big + 1)->quantity == 5);
        assert(b.validate({big, Side::Buy, OrderType::Limit, 10000, 5}) == RejectReason::DuplicateId);
        assert(b.cancelOrder(big) && !b.hasOrder(big));
        auto t = b.addOrder({7, Side::Sell, OrderType::Market, 0, 5});
        assert(t.size() == 1 && t[0].buyId == big + 1);
    }
    // 38. A book that outgrows its reserveOrders hint still works, and its ids can be reused
    {
        Book b;
        b.reserveOrders(4);
        for (OrderId id = 1; id <= 3000; ++id)
            b.addOrder({id, id % 2 ? Side::Buy : Side::Sell, OrderType::Limit,
                        static_cast<Price>(id % 2 ? 9999 - id % 50 : 10001 + id % 50), 2});
        for (OrderId id = 1; id <= 3000; ++id) assert(b.hasOrder(id));
        for (OrderId id = 1; id <= 3000; id += 3) assert(b.cancelOrder(id));
        for (OrderId id = 1; id <= 3000; ++id) assert(b.hasOrder(id) == (id % 3 != 1));
        for (OrderId id = 1; id <= 3000; id += 3) b.addOrder({id, Side::Buy, OrderType::Limit, 9000, 1});
        assert(b.depth(Side::Buy, 1000).back().price == 9000 && b.depth(Side::Buy, 1000).back().orderCount == 1000);
        assert(b.tradeHistory().empty());
    }
    // 39. reserveTrades keeps the existing history and doesn't invent trades
    {
        Book b;
        b.addOrder({1, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({2, Side::Buy, OrderType::Limit, 10000, 5});
        b.reserveTrades(100000);
        assert(b.tradeHistory().size() == 1 && b.tradeHistory()[0].sellId == 1);
        b.addOrder({3, Side::Sell, OrderType::Limit, 10000, 5});
        b.addOrder({4, Side::Buy, OrderType::Limit, 10000, 5});
        assert(b.tradeHistory().size() == 2 && b.tradeHistory()[1].sellId == 3);
    }
}

// ---- Phase 4: differential test ----
// Run one generated order flow through every engine and check that each one produces
// exactly the same trades and ends with exactly the same book as engine A.

bool sameTrades(const std::vector<Trade>& x, const std::vector<Trade>& y) {
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i)        // timestamps are clock readings, so they aren't compared
        if (x[i].buyId != y[i].buyId || x[i].sellId != y[i].sellId ||
            x[i].price != y[i].price || x[i].quantity != y[i].quantity) return false;
    return true;
}

bool sameBook(const OrderBookBase& x, const OrderBookBase& y, OrderId maxId) {
    if (x.bestBid() != y.bestBid() || x.bestAsk() != y.bestAsk()) return false;
    for (Side side : {Side::Buy, Side::Sell}) {
        auto dx = x.depth(side, static_cast<std::size_t>(-1)), dy = y.depth(side, static_cast<std::size_t>(-1));
        if (dx.size() != dy.size()) return false;
        for (std::size_t i = 0; i < dx.size(); ++i)
            if (dx[i].price != dy[i].price || dx[i].quantity != dy[i].quantity ||
                dx[i].orderCount != dy[i].orderCount) return false;
    }
    for (OrderId id = 1; id <= maxId; ++id) {          // every order, one by one
        auto ox = x.getOrder(id), oy = y.getOrder(id);
        if (ox.has_value() != oy.has_value() || x.hasOrder(id) != y.hasOrder(id)) return false;
        if (ox && (ox->side != oy->side || ox->price != oy->price || ox->quantity != oy->quantity)) return false;
    }
    return true;
}

void differentialTest(std::uint64_t seed, std::size_t bookSize, std::size_t actions) {
    FlowConfig cfg;
    cfg.seed = seed;
    cfg.targetBookSize = bookSize;
    OrderFlowGenerator gen(cfg);
    auto prefill = gen.prefill();
    auto flow    = gen.generate(actions);

    OrderId maxId = 0;
    for (const Action& a : prefill) maxId = std::max(maxId, a.order.id);
    for (const Action& a : flow)    maxId = std::max(maxId, a.order.id);

    std::vector<std::unique_ptr<OrderBookBase>> books;
    for (const std::string& name : engineNames()) {
        books.push_back(makeBook(name));
        assert(books.back());
        for (const Action& a : prefill) apply(*books.back(), a);
        for (const Action& a : flow)    apply(*books.back(), a);
    }
    assert(!books[0]->tradeHistory().empty());

    for (std::size_t i = 1; i < books.size(); ++i) {
        assert(sameTrades(books[0]->tradeHistory(), books[i]->tradeHistory()));
        assert(sameBook(*books[0], *books[i], maxId));
    }

    // Empty both sides with two huge market orders. The trades list every resting order in
    // the order it fills, so this also proves the queues inside each level are identical.
    for (auto& book : books) {
        book->addOrder({maxId + 1, Side::Buy,  OrderType::Market, 0, 4000000000u});
        book->addOrder({maxId + 2, Side::Sell, OrderType::Market, 0, 4000000000u});
        assert(!book->bestBid() && !book->bestAsk());
    }
    for (std::size_t i = 1; i < books.size(); ++i)
        assert(sameTrades(books[0]->tradeHistory(), books[i]->tradeHistory()));
}

int main() {
    engineTests<OrderBook>();
    std::printf("A : all 39 tests passed.\n");
    engineTests<FlatBook>();
    std::printf("B : all 39 tests passed.\n");

    differentialTest(42, 200, 30000);
    differentialTest(7, 2000, 30000);
    differentialTest(2026, 20000, 60000);
    std::printf("Differential test passed: %zu engine(s) agree on every trade and on the final book.\n",
                engineNames().size());
}
