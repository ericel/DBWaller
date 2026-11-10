# tests/unit/CMakeLists.txt

find_package(Catch2 CONFIG REQUIRED)

set(DBWALLER_UNIT_TEST_TARGETS
	dbwaller_unit_core_engine
)

add_executable(dbwaller_unit_core_engine
	security/claims_tests.cpp
	policy/cache_key_tests.cpp
	policy/rules_tests.cpp
	adapters/request_context_tests.cpp
)

target_link_libraries(dbwaller_unit_core_engine PRIVATE
	dbwaller
	Catch2::Catch2WithMain
)

set_target_properties(dbwaller_unit_core_engine PROPERTIES FOLDER "tests/unit")

add_test(NAME dbwaller_unit_core_engine COMMAND dbwaller_unit_core_engine)
set_tests_properties(dbwaller_unit_core_engine PROPERTIES LABELS "unit")

add_executable(dbwaller_concurrency_tests
	concurrency/thread_pool_tests.cpp
	core/compute_lifecycle_tests.cpp
	core/gateway_lifetime_tests.cpp
)
target_link_libraries(dbwaller_concurrency_tests PRIVATE dbwaller Catch2::Catch2WithMain)
include(Catch)
catch_discover_tests(dbwaller_concurrency_tests
	TEST_PREFIX "concurrency:"
	TEST_LIST dbwaller_concurrency_cases
	PROPERTIES TIMEOUT 45
)
# Apply the list-valued LABELS property after Catch has discovered the cases.
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
	"${CMAKE_CURRENT_SOURCE_DIR}/concurrency/labels.cmake")
