#include <iostream>
#include <cstdlib>
#include <vector>
#include <string>

int main() {
    std::cout << "========================================\n";
    std::cout << "    DBWALLER: FULL SUITE EXECUTION      \n";
    std::cout << "========================================\n\n";

    // List of tests to run (order matters)
    std::vector<std::string> tests = {
        "./dbwaller_embedded_demo",
        "./dbwaller_test_isolation",
        "./dbwaller_test_invalidation",
        "./dbwaller_test_thundering_herd",
        "./dbwaller_test_eviction",
        "./dbwaller_test_swr_bench",
        "./dbwaller_benchmark_throughput"
    };

    int failures = 0;

    for (const auto& test : tests) {
        std::cout << ">>> RUNNING: " << test << " ...\n";
        std::cout << "----------------------------------------\n";
        
        // Use system() to run the separate executable
        int ret = std::system(test.c_str());
        
        std::cout << "----------------------------------------\n";
        if (ret == 0) {
            std::cout << ">>> [PASS] " << test << "\n\n";
        } else {
            std::cout << ">>> [FAIL] " << test << " (Exit Code: " << ret << ")\n\n";
            failures++;
        }
    }

    std::cout << "========================================\n";
    std::cout << "SUMMARY: " << (tests.size() - failures) << "/" << tests.size() << " Passed.\n";
    
    return failures > 0 ? 1 : 0;
}