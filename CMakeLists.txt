cmake_minimum_required(VERSION 3.15)
project(OrderBook CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/O2 /W4)
else()
    add_compile_options(-O2 -Wall -Wextra)
endif()

# The engine, compiled once and shared by both programs
add_library(orderbook STATIC OrderBook.cpp)

add_executable(demo main.cpp)
target_link_libraries(demo PRIVATE orderbook)

add_executable(tests tests.cpp)
target_link_libraries(tests PRIVATE orderbook)

# tests.cpp relies on assert(). A Release build defines NDEBUG, which turns every
# assert into nothing, so make sure NDEBUG is never defined for the tests.
if(MSVC)
    target_compile_options(tests PRIVATE /UNDEBUG)
else()
    target_compile_options(tests PRIVATE -UNDEBUG)
endif()

enable_testing()
add_test(NAME tests COMMAND tests)
