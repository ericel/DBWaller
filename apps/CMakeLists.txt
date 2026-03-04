# apps/CMakeLists.txt

# 1) Common app-only support (fake adapter, helpers, small utilities)
add_subdirectory(support)

# 2) Demo app (allowed to use fake adapter)
add_subdirectory(embedded_demo)

# 3) Real apps (should NOT depend on fake adapter unless you explicitly want demo mode)
add_subdirectory(dbwaller_daemon)
add_subdirectory(benchmark_harness)
