from conan import ConanFile
from conan.tools.cmake import cmake_layout

class DBWallerConan(ConanFile):
    name = "dbwaller"
    version = "0.1.0"

    settings = "os", "arch", "compiler", "build_type"
    options = {
        "with_tests": [True, False],
        "with_benchmarks": [True, False],
    }
    default_options = {
        "with_tests": False,
        "with_benchmarks": False,
        "spdlog/*:header_only": False,
    }

    def requirements(self):
        self.requires("spdlog/1.14.1")
        self.requires("openssl/3.3.2")

        if self.options.with_tests:
            self.requires("catch2/3.7.1")

        if self.options.with_benchmarks:
            # Pin to a version that tends to build cleanly in more environments.
            # If you still want 1.9.0 later, we can patch/override it.
            self.requires("benchmark/1.8.3")
        
        self.requires("nlohmann_json/3.11.3")
        
        

    generators = "CMakeDeps", "CMakeToolchain"

    def layout(self):
        cmake_layout(self)
