import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd, cross_building
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load


class DBWallerRemoteConan(ConanFile):
    name = "dbwaller-remote"
    package_type = "library"
    license = "MIT"
    description = "Asynchronous C++20 client for the DBWaller v1 cache service"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {
        "shared": False, "fPIC": True,
        "grpc/*:csharp_plugin": False, "grpc/*:node_plugin": False,
        "grpc/*:objective_c_plugin": False, "grpc/*:php_plugin": False,
        "grpc/*:python_plugin": False, "grpc/*:ruby_plugin": False,
    }

    def set_version(self):
        version = os.path.join(self.recipe_folder, "VERSION")
        if not os.path.exists(version):
            version = os.path.join(self.recipe_folder, "..", "..", "VERSION")
        self.version = load(self, version).strip()

    def export(self):
        copy(self, "VERSION", os.path.join(self.recipe_folder, "..", ".."), self.export_folder)

    def export_sources(self):
        root = os.path.abspath(os.path.join(self.recipe_folder, "..", ".."))
        for name in ("VERSION", "LICENSE"):
            copy(self, name, root, self.export_sources_folder)
        copy(self, "*", os.path.join(root, "proto"), os.path.join(self.export_sources_folder, "proto"))
        copy(self, "CMakeLists.txt", self.recipe_folder, self.export_sources_folder)
        for name in ("src", "include", "cmake"):
            copy(self, "*", os.path.join(self.recipe_folder, name), os.path.join(self.export_sources_folder, name))

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate(self):
        check_min_cppstd(self, "20")
        if str(self.settings.os) not in ("Linux", "Macos") or cross_building(self):
            raise ConanInvalidConfiguration("The remote SDK currently requires native Linux or macOS builds")

    def requirements(self):
        self.requires("grpc/1.84.0")
        self.requires("openssl/3.5.6")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        tc = CMakeToolchain(self)
        tc.user_presets_path = False
        tc.variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        tc.variables["CMAKE_POSITION_INDEPENDENT_CODE"] = bool(self.options.get_safe("fPIC", True))
        tc.variables["Protobuf_PROTOC_EXECUTABLE"] = os.path.join(self.dependencies["protobuf"].package_folder, "bin", "protoc")
        tc.variables["GRPC_CPP_PLUGIN_PROGRAM"] = os.path.join(self.dependencies["grpc"].package_folder, "bin", "grpc_cpp_plugin")
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        for dependency in self.dependencies.host.values():
            if dependency.package_folder:
                copy(self, "*", os.path.join(dependency.package_folder, "licenses"),
                     os.path.join(self.package_folder, "licenses", "dependencies", dependency.ref.name))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "DBWallerRemote")
        self.cpp_info.set_property("cmake_target_name", "DBWallerRemote::client")
        self.cpp_info.libs = ["dbwaller_remote"]
        if self.settings.os == "Linux":
            self.cpp_info.system_libs.append("pthread")
