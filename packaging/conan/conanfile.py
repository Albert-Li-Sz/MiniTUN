from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import apply_conandata_patches, copy, export_conandata_patches, get


class MiniTunConan(ConanFile):
    name = "minitun"
    version = "1.2.0"
    license = "MIT"
    url = "https://github.com/Albert-Li-Sz/MiniTUN"
    description = "A minimal-footprint, self-hosted multi-transport reverse tunnel"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}
    exports_sources = "CMakeLists.txt", "cmake/*", "include/*", "src/*", "apps/*", "abi/*"

    def requirements(self):
        self.requires("openssl/3.0.16")
        self.requires("sqlite3/3.50.3")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["MINITUN_USE_SYSTEM_DEPS"] = "OFF"
        toolchain.variables["MINITUN_BUILD_TESTS"] = "OFF"
        toolchain.variables["MINITUN_BUILD_PACKAGES"] = "OFF"
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", src=self.source_folder, dst=self.package_folder / "licenses")

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "MiniTun")
        self.cpp_info.components["client"].libs = ["minitun-client"]
        self.cpp_info.components["client"].set_property("cmake_target_name", "MiniTun::Client")
        self.cpp_info.components["protocol"].libs = ["minitun-remote-protocol"]
        self.cpp_info.components["protocol"].set_property("cmake_target_name", "MiniTun::RemoteProtocol")
