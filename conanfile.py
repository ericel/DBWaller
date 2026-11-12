import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd, cross_building
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
        "with_daemon": [True, False],
        "with_remote_tests": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_tests": False,
        "with_benchmarks": False,
        "build_apps": False,
        "with_daemon": False,
        "with_remote_tests": False,
        "spdlog/*:header_only": False,
        "grpc/*:csharp_plugin": False,
        "grpc/*:node_plugin": False,
        "grpc/*:objective_c_plugin": False,
        "grpc/*:php_plugin": False,
        "grpc/*:python_plugin": False,
        "grpc/*:ruby_plugin": False,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate(self):
        check_min_cppstd(self, "20")
        if self.options.with_remote_tests and not (self.options.with_daemon and self.options.with_tests):
            raise ConanInvalidConfiguration("Remote integration tests require with_daemon=True and with_tests=True")
        if self.options.with_daemon and str(self.settings.os) not in ("Linux", "Macos"):
            raise ConanInvalidConfiguration("The daemon currently supports Linux and macOS")
        if self.options.with_daemon and cross_building(self):
            raise ConanInvalidConfiguration("The daemon currently requires a native build for its code generators")

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

        for directory in ("apps", "benchmarks", "cmake", "docs", "examples", "include", "proto", "scripts", "sdk", "src", "tests"):
            copy(
                self,
                "*",
                os.path.join(self.recipe_folder, directory),
                os.path.join(self.export_sources_folder, directory),
                excludes=("build/*", "*/build/*", "__pycache__/*", "*/__pycache__/*",
                          "*.pyc", "*CMakeUserPresets.json"),
            )

    def requirements(self):
        self.requires("spdlog/1.15.3")
        self.requires("openssl/3.5.6")
        self.requires("nlohmann_json/3.12.0")
        if self.options.with_daemon:
            self.requires("grpc/1.84.0")

        if self.options.with_tests:
            self.requires("catch2/3.7.1")

        if self.options.with_benchmarks:
            self.requires("benchmark/1.9.1")

    def package_id(self):
        self.info.options.rm_safe("with_tests")
        self.info.options.rm_safe("with_benchmarks")
        self.info.options.rm_safe("build_apps")
        self.info.options.rm_safe("with_remote_tests")

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
        toolchain.variables["DBWALLER_BUILD_DAEMON"] = bool(self.options.with_daemon)
        toolchain.variables["DBWALLER_BUILD_REMOTE"] = bool(self.options.with_remote_tests)
        if self.options.with_daemon:
            # ConanCenter's imported executable targets otherwise search PATH,
            # which can pick up a system generator incompatible with this graph.
            toolchain.variables["Protobuf_PROTOC_EXECUTABLE"] = os.path.join(
                self.dependencies["protobuf"].package_folder, "bin", "protoc")
            toolchain.variables["GRPC_CPP_PLUGIN_PROGRAM"] = os.path.join(
                self.dependencies["grpc"].package_folder, "bin", "grpc_cpp_plugin")
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        if self.options.with_daemon:
            for dependency in self.dependencies.host.values():
                if dependency.package_folder:
                    copy(self, "*", os.path.join(dependency.package_folder, "licenses"),
                         os.path.join(self.package_folder, "licenses", "dependencies", dependency.ref.name))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "DBWaller")
        self.cpp_info.set_property("cmake_target_name", "DBWaller::dbwaller")
        self.cpp_info.libs = ["dbwaller"]
