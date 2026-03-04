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
