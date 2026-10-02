#include "Engines.hpp"
#include "FlatBook.hpp"
#include "IntrusiveBook.hpp"
#include "OrderBook.hpp"

const std::vector<std::string>& engineNames() {
    static const std::vector<std::string> names = {"A", "B", "C1", "C2", "C3", "C4", "D"};
    return names;
}

std::unique_ptr<OrderBookBase> makeBook(const std::string& name) {
    if (name == "A")  return std::make_unique<OrderBook>();
    if (name == "B")  return std::make_unique<FlatBook>();
    if (name == "C1") return std::make_unique<PoolBook>();
    if (name == "C2") return std::make_unique<IntrusiveBookC2>();
    if (name == "C3") return std::make_unique<IntrusiveBookC3>();
    if (name == "C4") return std::make_unique<CustomBook>();
    if (name == "D")  return std::make_unique<HybridBook>();
    return nullptr;
}
