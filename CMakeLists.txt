cmake_minimum_required(VERSION 3.24)

project(stock_exchange_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(EXCHANGE_BUILD_TESTS "Build the test suite" ON)
option(EXCHANGE_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(EXCHANGE_SANITIZE_THREAD "Build with ThreadSanitizer" OFF)

# Per-symbol capacities of the networked exchange (see src/exchange/config.h).
set(EXCHANGE_ORDER_CAPACITY "" CACHE STRING "Open orders per symbol")
set(EXCHANGE_LEVEL_CAPACITY "" CACHE STRING "Price levels per symbol")
set(EXCHANGE_EVENT_CAPACITY "" CACHE STRING "Events one inbound order may produce")
set(EXCHANGE_STOP_CAPACITY "" CACHE STRING "Parked stop orders per symbol")

find_package(Threads REQUIRED)

# Everything is header-only; this target carries the include path and flags.
add_library(stock_exchange_project INTERFACE)

target_include_directories(
    stock_exchange_project
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(
    stock_exchange_project
    INTERFACE
        cxx_std_20
)

target_compile_options(
    stock_exchange_project
    INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -fno-exceptions
)

target_link_libraries(
    stock_exchange_project
    INTERFACE
        Threads::Threads
)

foreach(capacity ORDER LEVEL EVENT STOP)
    if(EXCHANGE_${capacity}_CAPACITY)
        target_compile_definitions(
            stock_exchange_project
            INTERFACE
                EXCHANGE_${capacity}_CAPACITY=${EXCHANGE_${capacity}_CAPACITY}
        )
    endif()
endforeach()

if(EXCHANGE_SANITIZE AND EXCHANGE_SANITIZE_THREAD)
    message(FATAL_ERROR "EXCHANGE_SANITIZE and EXCHANGE_SANITIZE_THREAD cannot be combined")
endif()

if(EXCHANGE_SANITIZE)
    target_compile_options(stock_exchange_project INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(stock_exchange_project INTERFACE -fsanitize=address,undefined)
endif()

if(EXCHANGE_SANITIZE_THREAD)
    target_compile_options(stock_exchange_project INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(stock_exchange_project INTERFACE -fsanitize=thread)
endif()

# add_exchange_executable(<name> <source> <output dir under the build tree>)
function(add_exchange_executable name source output_dir)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE stock_exchange_project)
    set_target_properties(
        ${name}
        PROPERTIES
            OUTPUT_NAME "${name}"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${output_dir}"
    )
endfunction()

add_exchange_executable(stock_exchange  src/stock_exchange_main.cpp src)
add_exchange_executable(trader_terminal tools/trader_terminal.cpp   tools)
add_exchange_executable(liquidity_bot   tools/liquidity_bot.cpp     tools)
add_exchange_executable(bench_engine    tools/bench_engine.cpp      tools)
add_exchange_executable(journal_dump    tools/journal_dump.cpp      tools)

if(EXCHANGE_BUILD_TESTS)
    enable_testing()

    foreach(test_name
            test_core
            test_order_book
            test_matching_engine
            test_fuzz
            test_no_alloc
            test_journal
            test_protocol
            test_exchange)
        add_exchange_executable(${test_name} tests/${test_name}.cpp tests)
        target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
    endforeach()

    # The sanitizers bring their own allocator hooks, which clash with the
    # counting operator new this test installs.
    if(EXCHANGE_SANITIZE OR EXCHANGE_SANITIZE_THREAD)
        set_tests_properties(test_no_alloc PROPERTIES DISABLED TRUE)
    endif()
endif()
