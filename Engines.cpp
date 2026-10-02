#include "Engines.hpp"
#include "FlatBook.hpp"
#include "OrderBook.hpp"

const std::vector<std::string>& engineNames() {
    static const std::vector<std::string> names = {"A", "B", "C1"};
    return names;
}

std::unique_ptr<OrderBookBase> makeBook(const std::string& name) {
    if (name == "A")  return std::make_unique<OrderBook>();
    if (name == "B")  return std::make_unique<FlatBook>();
    if (name == "C1") return std::make_unique<PoolBook>();
    return nullptr;
}
