if(dbwaller_concurrency_cases)
    set_tests_properties(${dbwaller_concurrency_cases}
        PROPERTIES LABELS "unit;regression;concurrency")
endif()
