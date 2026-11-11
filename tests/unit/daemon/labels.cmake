if(dbwaller_service_cases)
    set_tests_properties(${dbwaller_service_cases} PROPERTIES LABELS "unit;service;concurrency")
endif()
