# apps/support/CMakeLists.txt
# Interface library: header-only helpers used by demos/tests (by choice).
add_library(dbwaller_app_support INTERFACE)

# Expose apps/ as an include root so you can do:
#   #include "support/fake_adapter.hpp"
target_include_directories(dbwaller_app_support INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/..
)


# add_library(dbwaller::app_support ALIAS dbwaller_app_support)
# target_link_libraries(dbwaller_embedded_demo PRIVATE dbwaller::app_support)
