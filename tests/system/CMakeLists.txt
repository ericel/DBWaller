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
