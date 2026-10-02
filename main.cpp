#include "OrderBook.hpp"
#include <cstdio>

static void showTrades(const std::vector<Trade>& trades) {
    if (trades.empty()) { std::printf("  no trades\n"); return; }
    for (const Trade& t : trades)
        std::printf("  TRADE  %u @ $%lld.%02lld  (buy #%llu, sell #%llu)\n", t.quantity,
                    (long long)(t.price / 100), (long long)(t.price % 100),
                    (unsigned long long)t.buyId, (unsigned long long)t.sellId);
}

int main() {
    OrderBook book;

    // The starting book from the project spec
    book.addOrder({1, Side::Buy,  OrderType::Limit, 10100, 20});
    book.addOrder({2, Side::Buy,  OrderType::Limit, 10090, 35});
    book.addOrder({3, Side::Buy,  OrderType::Limit, 10080, 50});
    book.addOrder({4, Side::Sell, OrderType::Limit, 10120, 15});
    book.addOrder({5, Side::Sell, OrderType::Limit, 10130, 40});
    book.addOrder({6, Side::Sell, OrderType::Limit, 10140, 25});

    std::printf("Starting book:\n");
    book.print();

    std::printf("\n> BUY 10 @ $101.20 (limit)\n");
    showTrades(book.addOrder({7, Side::Buy, OrderType::Limit, 10120, 10}));

    std::printf("\n> BUY 30 @ $101.30 (limit, sweeps two levels)\n");
    showTrades(book.addOrder({8, Side::Buy, OrderType::Limit, 10130, 30}));

    std::printf("\n> SELL 40 (market)\n");
    showTrades(book.addOrder({9, Side::Sell, OrderType::Market, 0, 40}));

    std::printf("\n> BUY 5 @ $100.50 (limit, no match -> rests in book)\n");
    showTrades(book.addOrder({10, Side::Buy, OrderType::Limit, 10050, 5}));

    std::printf("\n> CANCEL #3\n  %s\n", book.cancelOrder(3) ? "cancelled" : "not found");

    std::printf("\nFinal book:\n");
    book.print();
    std::printf("\nTotal trades: %zu\n", book.tradeHistory().size());
}
