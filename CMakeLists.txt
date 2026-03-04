cmake_minimum_required(VERSION 3.20)
project(DBWaller VERSION 0.1.0 LANGUAGES CXX)

option(DBWALLER_BUILD_APPS       "Build DBWaller apps"       ON)
option(DBWALLER_BUILD_TESTS      "Build DBWaller tests"      OFF)
# REMOVED: option(DBWALLER_BUILD_BENCHMARKS ... OFF) - We don't need this toggle anymore if it implies Google Benchmark

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dbwaller STATIC
    src/concurrency/thread_pool.cpp
    src/core/sharded_engine.cpp
    src/security/crypto.cpp
    src/security/claims.cpp
    src/policy/config_loader.cpp
    src/observability/observer.cpp
)

add_library(DBWaller::dbwaller ALIAS dbwaller)

target_include_directories(dbwaller PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(dbwaller PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(dbwaller PUBLIC spdlog::spdlog)

find_package(OpenSSL REQUIRED)
target_link_libraries(dbwaller PRIVATE OpenSSL::Crypto)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(dbwaller PUBLIC nlohmann_json::nlohmann_json)

if(DBWALLER_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(DBWALLER_BUILD_TESTS)
    include(CTest)
    enable_testing()

    if(NOT TARGET dbwaller_app_support)
        add_subdirectory(apps/support apps_support_for_tests)
    endif()

    add_subdirectory(tests)
endif()

# Option can remain, but default to ON if you want
#option(DBWALLER_BUILD_BENCHMARKS "Build DBWaller benchmarks" ON)

# if(DBWALLER_BUILD_BENCHMARKS)
#     # find_package(benchmark CONFIG REQUIRED)  <-- REMOVE THIS
#     add_subdirectory(benchmarks)
# endif()
if(DBWALLER_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
    add_subdirectory(benchmarks)
endif()

# cmake_minimum_required(VERSION 3.20)
# project(DBWaller VERSION 0.1.0 LANGUAGES CXX)

# option(DBWALLER_BUILD_APPS       "Build DBWaller apps"       ON)
# option(DBWALLER_BUILD_TESTS      "Build DBWaller tests"      OFF)
# option(DBWALLER_BUILD_BENCHMARKS "Build DBWaller benchmarks" OFF)

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)
# set(CMAKE_CXX_EXTENSIONS OFF)

# add_library(dbwaller STATIC
#     src/concurrency/thread_pool.cpp
#     src/core/sharded_engine.cpp
#     src/security/crypto.cpp
#     src/security/claims.cpp
#     src/policy/config_loader.cpp
# )

# add_library(DBWaller::dbwaller ALIAS dbwaller)

# target_include_directories(dbwaller PUBLIC
#     ${CMAKE_CURRENT_SOURCE_DIR}/include
# )

# target_compile_options(dbwaller PRIVATE
#     $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
#     $<$<CXX_COMPILER_ID:MSVC>:/W4>
# )

# find_package(spdlog CONFIG REQUIRED)
# target_link_libraries(dbwaller PUBLIC spdlog::spdlog)

# find_package(OpenSSL REQUIRED)
# target_link_libraries(dbwaller PRIVATE OpenSSL::Crypto)

# find_package(nlohmann_json CONFIG REQUIRED)
# target_link_libraries(dbwaller PUBLIC nlohmann_json::nlohmann_json)

# if(DBWALLER_BUILD_APPS)
#     add_subdirectory(apps)
# endif()

# if(DBWALLER_BUILD_TESTS)
#     include(CTest)
#     enable_testing()

#     if(NOT TARGET dbwaller_app_support)
#         add_subdirectory(apps/support apps_support_for_tests)
#     endif()

#     add_subdirectory(tests)
# endif()

# if(DBWALLER_BUILD_BENCHMARKS)
#     find_package(benchmark CONFIG REQUIRED)
#     add_subdirectory(benchmarks)
# endif()




# cmake_minimum_required(VERSION 3.20)
# project(DBWaller VERSION 0.1.0 LANGUAGES CXX)

# option(DBWALLER_BUILD_APPS       "Build DBWaller apps"       ON)
# option(DBWALLER_BUILD_TESTS      "Build DBWaller tests"      OFF)
# option(DBWALLER_BUILD_BENCHMARKS "Build DBWaller benchmarks" OFF)

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)
# set(CMAKE_CXX_EXTENSIONS OFF)

# add_library(dbwaller STATIC
#     src/concurrency/thread_pool.cpp
#     src/core/sharded_engine.cpp
#     src/security/crypto.cpp
#     src/security/claims.cpp
#     src/policy/config_loader.cpp
# )

# target_include_directories(dbwaller PUBLIC
#     ${CMAKE_CURRENT_SOURCE_DIR}/include
# )

# # Keep warnings for library + anything linking to it (nice for tests/benches too)
# target_compile_options(dbwaller PUBLIC
#     -Wall -Wextra -Wpedantic
# )

# # ---- Runtime deps used by the library ----
# find_package(spdlog CONFIG REQUIRED)
# # If you truly want header-only, use:
# # target_link_libraries(dbwaller PUBLIC spdlog::spdlog_header_only)
# # Otherwise compiled lib:
# target_link_libraries(dbwaller PUBLIC spdlog::spdlog)

# find_package(OpenSSL REQUIRED)
# target_link_libraries(dbwaller PRIVATE OpenSSL::Crypto)

# find_package(nlohmann_json CONFIG REQUIRED)
# target_link_libraries(dbwaller PUBLIC nlohmann_json::nlohmann_json)

# # ---- Apps / Tests / Benchmarks ----
# if(DBWALLER_BUILD_APPS)
#     add_subdirectory(apps)
# endif()

# if(DBWALLER_BUILD_TESTS)
#     include(CTest)
#     enable_testing()

#     # If apps are OFF, apps/support wasn't added, but tests still need fake_adapter.
#     # If apps are ON, apps/support already ran via add_subdirectory(apps) -> no duplicate.
#     if(NOT TARGET dbwaller_app_support)
#         add_subdirectory(apps/support apps_support_for_tests) # unique binary dir
#     endif()

#     add_subdirectory(tests)
# endif()
# if(DBWALLER_BUILD_BENCHMARKS)
#     add_subdirectory(benchmarks)
# endif()





# cmake_minimum_required(VERSION 3.20)
# project(DBWaller VERSION 0.1.0 LANGUAGES CXX)

# option(DBWALLER_BUILD_APPS "Build DBWaller apps" ON)
# option(DBWALLER_BUILD_TESTS "Build DBWaller tests" OFF)
# option(DBWALLER_BUILD_BENCHMARKS "Build DBWaller benchmarks" OFF)

# set(CMAKE_CXX_STANDARD 20)
# set(CMAKE_CXX_STANDARD_REQUIRED ON)
# set(CMAKE_CXX_EXTENSIONS OFF)

# add_library(dbwaller STATIC
#     src/concurrency/thread_pool.cpp
#     src/core/sharded_engine.cpp
#     src/security/crypto.cpp
#     src/security/claims.cpp
#     src/policy/config_loader.cpp
# )

# target_include_directories(dbwaller PUBLIC
#     ${CMAKE_CURRENT_SOURCE_DIR}/include
# )

# target_compile_options(dbwaller PRIVATE
#     -Wall -Wextra -Wpedantic
# )

# # ---- Runtime deps used by the library ----
# find_package(spdlog CONFIG REQUIRED)
# target_link_libraries(dbwaller PUBLIC spdlog::spdlog)

# find_package(OpenSSL REQUIRED)
# target_link_libraries(dbwaller PRIVATE OpenSSL::Crypto)

# find_package(nlohmann_json CONFIG REQUIRED)
# target_link_libraries(dbwaller PUBLIC nlohmann_json::nlohmann_json)


# # ---- Apps / Tests / Benchmarks ----
# if(DBWALLER_BUILD_APPS)
#     add_subdirectory(apps)
# endif()

# if(DBWALLER_BUILD_TESTS)
#     enable_testing()
#     add_subdirectory(tests)
# endif()

# if(DBWALLER_BUILD_BENCHMARKS)
#     add_subdirectory(benchmarks)
# endif()
