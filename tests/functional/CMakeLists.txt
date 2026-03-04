# tests/functional/CMakeLists.txt

set(DBWALLER_FUNCTIONAL_TEST_TARGETS
	dbwaller_test_isolation
	dbwaller_test_invalidation
	dbwaller_test_thundering_herd
	dbwaller_test_swr_bench
)

add_executable(dbwaller_test_isolation       ../integration/test_claims_isolation.cpp)
add_executable(dbwaller_test_invalidation    ../integration/test_invalidation.cpp)
add_executable(dbwaller_test_thundering_herd ../integration/test_thundering_herd.cpp)
add_executable(dbwaller_test_swr_bench       ../integration/test_swr_bench.cpp)

foreach(t IN LISTS DBWALLER_FUNCTIONAL_TEST_TARGETS)
	target_link_libraries(${t} PRIVATE
		dbwaller
		dbwaller_app_support
	)

	set_target_properties(${t} PROPERTIES FOLDER "tests/functional")

	add_test(NAME ${t} COMMAND ${t})

	set(test_labels functional integration)
	if(t STREQUAL "dbwaller_test_swr_bench")
		list(APPEND test_labels regression)
	endif()

	set_tests_properties(${t} PROPERTIES LABELS "${test_labels}")
endforeach()
