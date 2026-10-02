#include "Engines.hpp"
#include "OrderBook.hpp"

const std::vector<std::string>& engineNames() {
    static const std::vector<std::string> names = {"A"};
    return names;
}

std::unique_ptr<OrderBookBase> makeBook(const std::string& name) {
    if (name == "A") return std::make_unique<OrderBook>();
    return nullptr;
}
