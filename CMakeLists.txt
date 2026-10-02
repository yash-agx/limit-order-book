cmake_minimum_required(VERSION 3.15)
project(OrderBook CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Flags are set per target (not with CMAKE_BUILD_TYPE) so one build folder can hold both
# the normal -O2 programs and the -O3 benchmark.
if(MSVC)
    add_compile_options(/W4)
    set(OPT_NORMAL /O2)
    set(OPT_BENCH  /O2 /DNDEBUG)
    set(KEEP_ASSERTS /UNDEBUG)
else()
    add_compile_options(-Wall -Wextra)
    set(OPT_NORMAL -O2)
    set(OPT_BENCH  -O3 -DNDEBUG)      # what a CMake "Release" build uses
    set(KEEP_ASSERTS -UNDEBUG)
endif()

set(ENGINE_SOURCES OrderBookBase.cpp OrderBook.cpp FlatBook.cpp Engines.cpp OrderFlow.cpp)

# The engine, compiled once and shared by the demo and the tests
add_library(orderbook STATIC ${ENGINE_SOURCES})
target_compile_options(orderbook PUBLIC ${OPT_NORMAL})

add_executable(demo main.cpp)
target_link_libraries(demo PRIVATE orderbook)

# tests.cpp relies on assert(). Defining NDEBUG turns every assert into nothing,
# so make sure it is never defined for the tests.
add_executable(tests tests.cpp)
target_link_libraries(tests PRIVATE orderbook)
target_compile_options(tests PRIVATE ${KEEP_ASSERTS})

enable_testing()
add_test(NAME tests COMMAND tests)

# The benchmark does not use the library above: it compiles its own copy of the engine
# with release-mode flags, so the tests and the benchmark never share object files.
# It uses the Windows API for memory measurement, so it is Windows-only.
if(WIN32)
    add_executable(bench bench.cpp ${ENGINE_SOURCES})
    target_compile_options(bench PRIVATE ${OPT_BENCH})
    target_link_libraries(bench PRIVATE psapi)
endif()
