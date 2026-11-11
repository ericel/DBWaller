add_library(dbwaller_service STATIC src/cache_service.cpp)
target_compile_features(dbwaller_service PUBLIC cxx_std_20)
target_include_directories(dbwaller_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dbwaller_service PUBLIC dbwaller)
target_compile_options(dbwaller_service PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)
# Server-internal foundation; not installed as the future public remote SDK.

if(DBWALLER_BUILD_DAEMON)
    if(WIN32)
        message(FATAL_ERROR "The daemon currently supports Linux and macOS")
    endif()
    find_package(Protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)
    set(proto_root "${PROJECT_SOURCE_DIR}/proto")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${generated}")
    set(proto_file "${proto_root}/dbwaller/v1/cache.proto")
    add_custom_command(
        OUTPUT "${generated}/dbwaller/v1/cache.pb.cc" "${generated}/dbwaller/v1/cache.pb.h"
               "${generated}/dbwaller/v1/cache.grpc.pb.cc" "${generated}/dbwaller/v1/cache.grpc.pb.h"
        COMMAND protobuf::protoc --proto_path=${proto_root} --cpp_out=${generated}
                --grpc_out=${generated} --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
                "${proto_file}"
        DEPENDS "${proto_file}" protobuf::protoc gRPC::grpc_cpp_plugin VERBATIM)
    add_library(dbwaller_wire STATIC
        "${generated}/dbwaller/v1/cache.pb.cc" "${generated}/dbwaller/v1/cache.grpc.pb.cc")
    target_include_directories(dbwaller_wire PUBLIC "${generated}")
    target_link_libraries(dbwaller_wire PUBLIC gRPC::grpc++ protobuf::libprotobuf)
    add_library(dbwaller_rpc STATIC src/server_config.cpp src/rpc_service.cpp src/daemon.cpp)
    target_include_directories(dbwaller_rpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(dbwaller_rpc PUBLIC dbwaller_service dbwaller_wire
        PRIVATE nlohmann_json::nlohmann_json OpenSSL::SSL)
    target_compile_definitions(dbwaller_rpc PRIVATE DBWALLER_BUILD_VERSION="${PROJECT_VERSION}")
    target_compile_options(dbwaller_rpc PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)
    add_executable(dbwallerd src/main.cpp)
    target_link_libraries(dbwallerd PRIVATE dbwaller_rpc)
    install(TARGETS dbwallerd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES "${proto_file}" DESTINATION ${CMAKE_INSTALL_DATADIR}/DBWaller/proto/dbwaller/v1)
    install(FILES "${PROJECT_SOURCE_DIR}/docs/daemon.md"
                  "${PROJECT_SOURCE_DIR}/docs/bridge-service.md"
                  "${PROJECT_SOURCE_DIR}/docs/compute-lifecycle.md"
                  "${PROJECT_SOURCE_DIR}/docs/cache-invalidation.md"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/DBWaller/docs)
    install(FILES "${PROJECT_SOURCE_DIR}/scripts/create_daemon_config.py" DESTINATION ${CMAKE_INSTALL_DATADIR}/DBWaller/tools)
endif()
