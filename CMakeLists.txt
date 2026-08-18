cmake_minimum_required(VERSION 3.24)

project(stock_exchange_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


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


add_executable(
    stock_exchange
    src/stock_exchange_main.cpp
)

target_link_libraries(
    stock_exchange
    PRIVATE
        stock_exchange_project
)

set_target_properties(
    stock_exchange
    PROPERTIES
        OUTPUT_NAME "stock_exchange"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src"
)

add_executable(
    trader_terminal
    tools/trader_terminal.cpp
)

target_link_libraries(
    trader_terminal
    PRIVATE
        stock_exchange_project
)

set_target_properties(
    trader_terminal
    PROPERTIES
        OUTPUT_NAME "trader_terminal"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

add_executable(
    liquidity_bot
    tools/liquidity_bot.cpp
)

target_link_libraries(
    liquidity_bot
    PRIVATE
        stock_exchange_project
)

set_target_properties(
    liquidity_bot
    PROPERTIES
        OUTPUT_NAME "liquidity_bot"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)