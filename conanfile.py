import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load


class DBWallerConan(ConanFile):
    name = "dbwaller"
    package_type = "library"
    license = "MIT"
    description = "C++20 data-plane gateway with policy-aware caching and access controls"
    topics = ("c++", "conan", "cache", "gateway", "security")

    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_tests": [True, False],
        "with_benchmarks": [True, False],
        "build_apps": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_tests": False,
        "with_benchmarks": False,
        "build_apps": False,
        "spdlog/*:header_only": False,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate(self):
        check_min_cppstd(self, "20")

    def set_version(self):
        self.version = load(self, os.path.join(self.recipe_folder, "VERSION")).strip()

    def export_sources(self):
        files_to_copy = (
            "CMakeLists.txt",
            "CMakePresets.json",
            "LICENSE",
            "README.md",
            "VERSION",
            "policy.json",
        )

        for filename in files_to_copy:
            copy(self, filename, self.recipe_folder, self.export_sources_folder)

        for directory in ("apps", "benchmarks", "cmake", "include", "src", "tests"):
            copy(
                self,
                "*",
                os.path.join(self.recipe_folder, directory),
                os.path.join(self.export_sources_folder, directory),
            )

    def requirements(self):
        self.requires("spdlog/1.14.1")
        self.requires("openssl/3.3.2")
        self.requires("nlohmann_json/3.11.3")

        if self.options.with_tests:
            self.requires("catch2/3.7.1")

        if self.options.with_benchmarks:
            self.requires("benchmark/1.8.3")

    def package_id(self):
        self.info.options.rm_safe("with_tests")
        self.info.options.rm_safe("with_benchmarks")
        self.info.options.rm_safe("build_apps")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = False
        toolchain.variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        toolchain.variables["CMAKE_POSITION_INDEPENDENT_CODE"] = bool(self.options.get_safe("fPIC", True))
        toolchain.variables["DBWALLER_BUILD_APPS"] = bool(self.options.build_apps)
        toolchain.variables["DBWALLER_BUILD_TESTS"] = bool(self.options.with_tests)
        toolchain.variables["DBWALLER_BUILD_BENCHMARKS"] = bool(self.options.with_benchmarks)
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "DBWaller")
        self.cpp_info.set_property("cmake_target_name", "DBWaller::dbwaller")
        self.cpp_info.libs = ["dbwaller"]
