cmake_minimum_required(VERSION 3.20)
project(DBWaller VERSION 0.1.0 LANGUAGES CXX)


option(DBWALLER_BUILD_APPS "Build DBWaller apps" ON)
option(DBWALLER_BUILD_TESTS "Build DBWaller tests" ON)
option(DBWALLER_BUILD_BENCHMARKS "Build DBWaller benchmarks" ON)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


add_library(dbwaller STATIC
src/concurrency/thread_pool.cpp
)


target_include_directories(dbwaller PUBLIC
${CMAKE_CURRENT_SOURCE_DIR}/include
)


target_compile_options(dbwaller PRIVATE
-Wall -Wextra -Wpedantic
)


if(DBWALLER_BUILD_APPS)
add_subdirectory(apps)
endif()


if(DBWALLER_BUILD_TESTS)
enable_testing()
add_subdirectory(tests)
endif()


if(DBWALLER_BUILD_BENCHMARKS)
add_subdirectory(benchmarks)
endif()