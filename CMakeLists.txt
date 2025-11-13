cmake_minimum_required(VERSION 3.30)
project(binance_futures_zorro_plugin)

set(CMAKE_CXX_FLAGS_RELEASE "/Ox")

set(CMAKE_CXX_STANDARD 20)
add_definitions(-D_WIN32_WINNT=0x0A00 /utf-8)

find_package(spdlog CONFIG REQUIRED)
find_package(magic_enum REQUIRED)

include_directories(include binance_cpp_api/include binance_cpp_api/vk_cpp_common/include)
add_subdirectory(binance_cpp_api)

add_library(BinanceFutures SHARED dllmain.cpp src/binance_futures.cpp src/stdafx.cpp)
target_compile_definitions(BinanceFutures PUBLIC BINANCE_FUTURES_DLL_EXPORTS)

target_link_libraries(BinanceFutures PRIVATE spdlog::spdlog binance_api)