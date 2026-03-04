# apps/embedded_demo/CMakeLists.txt

add_executable(dbwaller_embedded_demo main.cpp)

target_link_libraries(dbwaller_embedded_demo PRIVATE
  dbwaller
  dbwaller_app_support   # gives access to support/fake_adapter.hpp
)

# Optional (but nice): warnings per-app
target_compile_options(dbwaller_embedded_demo PRIVATE -Wall -Wextra -Wpedantic)
