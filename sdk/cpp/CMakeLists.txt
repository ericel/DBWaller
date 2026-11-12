cmake_minimum_required(VERSION 3.20)

# The same source tree works as a repository subdirectory and as a standalone
# Conan source export. VERSION/proto are copied beside this file in the export.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    set(remote_root "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    get_filename_component(remote_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
file(READ "${remote_root}/VERSION" remote_version)
string(STRIP "${remote_version}" remote_version)
project(DBWallerRemote VERSION ${remote_version} LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(WIN32)
    message(FATAL_ERROR "The remote SDK currently supports native Linux and macOS builds")
endif()
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated}")
set(schema "${remote_root}/proto/dbwaller/v1/cache.proto")
if(NOT TARGET dbwaller_wire)
    add_custom_command(
        OUTPUT "${generated}/dbwaller/v1/cache.pb.cc" "${generated}/dbwaller/v1/cache.pb.h"
               "${generated}/dbwaller/v1/cache.grpc.pb.cc" "${generated}/dbwaller/v1/cache.grpc.pb.h"
        COMMAND protobuf::protoc --proto_path=${remote_root}/proto --cpp_out=${generated}
                --grpc_out=${generated} --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin> "${schema}"
        DEPENDS "${schema}" protobuf::protoc gRPC::grpc_cpp_plugin VERBATIM)
    set(remote_wire_sources "${generated}/dbwaller/v1/cache.pb.cc" "${generated}/dbwaller/v1/cache.grpc.pb.cc")
endif()
# Generated implementation is private and bundled into the one SDK archive.
add_library(dbwaller_remote src/client.cpp ${remote_wire_sources})
add_library(DBWallerRemote::client ALIAS dbwaller_remote)
set_target_properties(dbwaller_remote PROPERTIES EXPORT_NAME client POSITION_INDEPENDENT_CODE ON)
target_compile_features(dbwaller_remote PUBLIC cxx_std_20)
target_include_directories(dbwaller_remote
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE "${generated}")
target_link_libraries(dbwaller_remote PRIVATE gRPC::grpc++ protobuf::libprotobuf OpenSSL::SSL Threads::Threads)
if(TARGET dbwaller_wire)
    # In-process integration tests share the daemon's generated definitions.
    target_link_libraries(dbwaller_remote PRIVATE dbwaller_wire)
endif()
target_compile_options(dbwaller_remote PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    install(TARGETS dbwaller_remote EXPORT DBWallerRemoteTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES "${remote_root}/LICENSE" "${remote_root}/VERSION" DESTINATION ${CMAKE_INSTALL_DATADIR}/DBWallerRemote)
    configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DBWallerRemoteConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/DBWallerRemoteConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DBWallerRemote")
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/DBWallerRemoteConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/DBWallerRemoteConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/DBWallerRemoteConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DBWallerRemote")
    install(EXPORT DBWallerRemoteTargets NAMESPACE DBWallerRemote:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DBWallerRemote")
endif()
