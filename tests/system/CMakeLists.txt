# tests/system/CMakeLists.txt

set(DBWALLER_SYSTEM_TEST_TARGETS
	dbwaller_test_eviction
)

add_executable(dbwaller_test_eviction ../integration/test_eviction.cpp)

foreach(t IN LISTS DBWALLER_SYSTEM_TEST_TARGETS)
	target_link_libraries(${t} PRIVATE
		dbwaller
		dbwaller_app_support
	)

	set_target_properties(${t} PROPERTIES FOLDER "tests/system")

	add_test(NAME ${t} COMMAND ${t})
	set_tests_properties(${t} PROPERTIES LABELS "system;integration;slow")
endforeach()

if(DBWALLER_BUILD_DAEMON)
    find_package(Catch2 CONFIG REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(DBWALLER_OPENSSL_COMMAND openssl REQUIRED)
    add_executable(dbwaller_daemon_network daemon/network_tests.cpp)
    target_link_libraries(dbwaller_daemon_network PRIVATE dbwaller_rpc Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
    add_executable(dbwaller_daemon_probe daemon/probe.cpp)
    target_link_libraries(dbwaller_daemon_probe PRIVATE dbwaller_wire)
    set(fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/daemon-fixtures")
    add_test(NAME daemon_fixtures COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/daemon/process_tests.py"
        fixtures "${fixture_dir}" "${DBWALLER_OPENSSL_COMMAND}")
    set_tests_properties(daemon_fixtures PROPERTIES FIXTURES_SETUP daemon_tls LABELS "system;daemon" TIMEOUT 30)
    add_test(NAME daemon_network COMMAND dbwaller_daemon_network --reporter compact)
    set_tests_properties(daemon_network PROPERTIES FIXTURES_REQUIRED daemon_tls LABELS "system;daemon;concurrency"
        ENVIRONMENT "DBWALLER_TEST_DIR=${fixture_dir}" TIMEOUT 90)
    add_test(NAME daemon_process COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/daemon/process_tests.py"
        process "${fixture_dir}" "$<TARGET_FILE:dbwallerd>" "$<TARGET_FILE:dbwaller_daemon_probe>")
    set_tests_properties(daemon_process PROPERTIES FIXTURES_REQUIRED daemon_tls LABELS "system;daemon" TIMEOUT 90)
    if(DBWALLER_BUILD_REMOTE)
        add_executable(dbwaller_remote_tests remote/client_tests.cpp)
        target_link_libraries(dbwaller_remote_tests PRIVATE DBWallerRemote::client dbwaller_rpc Catch2::Catch2WithMain)
        target_compile_definitions(dbwaller_remote_tests PRIVATE DBWALLER_EXAMPLE_EMBEDDED)
        add_test(NAME remote_client COMMAND dbwaller_remote_tests --reporter compact)
        set_tests_properties(remote_client PROPERTIES FIXTURES_REQUIRED daemon_tls LABELS "system;remote;concurrency"
            ENVIRONMENT "DBWALLER_TEST_DIR=${fixture_dir}" TIMEOUT 90)
        add_executable(dbwaller_remote_probe remote/probe.cpp)
        target_link_libraries(dbwaller_remote_probe PRIVATE DBWallerRemote::client)
        add_test(NAME remote_process COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/daemon/process_tests.py"
            process "${fixture_dir}" "$<TARGET_FILE:dbwallerd>" "$<TARGET_FILE:dbwaller_remote_probe>")
        set_tests_properties(remote_process PROPERTIES FIXTURES_REQUIRED daemon_tls LABELS "system;remote" TIMEOUT 90
            RESOURCE_LOCK daemon_process_fixture)
        set_tests_properties(daemon_process PROPERTIES RESOURCE_LOCK daemon_process_fixture)
    endif()
endif()
