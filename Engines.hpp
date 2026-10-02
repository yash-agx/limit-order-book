#pragma once
#include "OrderBookBase.hpp"
#include <memory>
#include <string>
#include <vector>

// Every engine has a short name. This is the one place that knows how to build each of them.
//
//   A   baseline: std::map + std::list + std::unordered_map
//   B   flat array of price levels + vector queues + array index (FlatBook)
//   C1  A + memory pool (PoolBook)
//   C2  C1 + intrusive linked list, orders in one pre-allocated array (IntrusiveBook)
//   C3  C2 + open-addressing hash map for the id index
//   C4  C3 + cache-friendly node layout: the complete engine C (CustomBook)
const std::vector<std::string>& engineNames();

// Build an engine by name. Returns nullptr if the name is unknown.
std::unique_ptr<OrderBookBase> makeBook(const std::string& name);
