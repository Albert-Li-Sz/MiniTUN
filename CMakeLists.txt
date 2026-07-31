cmake_minimum_required(VERSION 3.22)

project(
    MiniTun
    VERSION 0.1.0
    DESCRIPTION "A small, high-performance TCP reverse-tunnelling system"
    HOMEPAGE_URL "https://github.com/LMTINSUZHOU/MiniTUN"
    LANGUAGES CXX
)

include(CTest)
include(GNUInstallDirs)

option(MINITUN_USE_SYSTEM_DEPS "Use distribution-provided dependencies" ON)
option(MINITUN_BUILD_TESTS "Build the MiniTun test suite" ON)
option(MINITUN_BUILD_FUZZERS "Build fuzz targets" OFF)
option(MINITUN_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MINITUN_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MINITUN_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MINITUN_ENABLE_LTO "Enable interprocedural optimization" OFF)
option(MINITUN_BUILD_PACKAGES "Enable package-generation support" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Dependencies)
include(Sanitizers)
include(Version)

minitun_setup_dependencies()

if(MINITUN_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT minitun_lto_supported OUTPUT minitun_lto_error)
    if(NOT minitun_lto_supported)
        message(FATAL_ERROR "LTO is unavailable: ${minitun_lto_error}")
    endif()
endif()

function(minitun_apply_lto target)
    if(MINITUN_ENABLE_LTO)
        set_property(TARGET "${target}" PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

add_library(minitun_common STATIC
    src/common/error.cpp
    src/common/logging.cpp
    src/common/version.cpp
)
add_library(MiniTun::common ALIAS minitun_common)

target_include_directories(minitun_common
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(minitun_common
    PUBLIC
        spdlog::spdlog
    PRIVATE
        MiniTun::dependencies
        Threads::Threads
        minitun_project_warnings
)
minitun_apply_sanitizers(minitun_common)
minitun_apply_version_definitions(minitun_common)
minitun_apply_lto(minitun_common)

function(minitun_add_program target source)
    add_executable("${target}" "${source}")
    target_link_libraries("${target}"
        PRIVATE
            MiniTun::common
            CLI11::CLI11
            minitun_project_warnings
    )
    minitun_apply_sanitizers("${target}")
    minitun_apply_version_definitions("${target}")
    minitun_apply_lto("${target}")
endfunction()

minitun_add_program(minitun apps/minitun_main.cpp)
minitun_add_program(minitund apps/minitund_main.cpp)
minitun_add_program(minitun-server apps/minitun_server_main.cpp)

if(MINITUN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MINITUN_BUILD_FUZZERS)
    message(STATUS "Fuzz targets will be introduced in protocol phase 5")
endif()

if(MINITUN_BUILD_PACKAGES)
    message(STATUS "Package generation will be introduced in packaging phase 14")
endif()
