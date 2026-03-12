cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DBWALLER_VERSION)
string(STRIP "${DBWALLER_VERSION}" DBWALLER_VERSION)

if(NOT DBWALLER_VERSION)
    message(FATAL_ERROR "VERSION file is empty")
endif()

project(DBWaller VERSION ${DBWALLER_VERSION} LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(DBWALLER_BUILD_APPS "Build DBWaller apps" ${PROJECT_IS_TOP_LEVEL})
option(DBWALLER_BUILD_TESTS "Build DBWaller tests" OFF)
option(DBWALLER_BUILD_BENCHMARKS "Build DBWaller benchmarks" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dbwaller
    src/concurrency/thread_pool.cpp
    src/core/sharded_engine.cpp
    src/observability/observer.cpp
    src/policy/config_loader.cpp
    src/security/claims.cpp
    src/security/crypto.cpp
)

add_library(DBWaller::dbwaller ALIAS dbwaller)

target_compile_features(dbwaller PUBLIC cxx_std_20)
set_target_properties(dbwaller PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(dbwaller
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_options(dbwaller PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

find_package(spdlog CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(dbwaller
    PUBLIC
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

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

if(DBWALLER_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
    add_subdirectory(benchmarks)
endif()

install(
    TARGETS dbwaller
    EXPORT DBWallerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    FILES LICENSE VERSION
    DESTINATION ${CMAKE_INSTALL_DATADIR}/DBWaller
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DBWallerConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DBWallerConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DBWaller"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/DBWallerConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/DBWallerConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/DBWallerConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DBWaller"
)

install(
    EXPORT DBWallerTargets
    FILE DBWallerTargets.cmake
    NAMESPACE DBWaller::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DBWaller"
)
