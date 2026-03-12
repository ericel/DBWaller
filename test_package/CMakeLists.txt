cmake_minimum_required(VERSION 3.20)
project(DBWallerTestPackage LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(DBWaller CONFIG REQUIRED)

add_executable(dbwaller_test_package main.cpp)
target_compile_features(dbwaller_test_package PRIVATE cxx_std_20)
target_link_libraries(dbwaller_test_package PRIVATE DBWaller::dbwaller)
