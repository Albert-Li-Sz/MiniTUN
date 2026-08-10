include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# renovate: datasource=github-releases depName=CLIUtils/CLI11
set(MINITUN_CLI11_VERSION "2.7.0")
# renovate: datasource=github-releases depName=nlohmann/json
set(MINITUN_NLOHMANN_JSON_VERSION "3.12.0")
# renovate: datasource=github-releases depName=gabime/spdlog
set(MINITUN_SPDLOG_VERSION "1.17.0")
# renovate: datasource=github-releases depName=google/googletest
set(MINITUN_GTEST_VERSION "1.17.0")
# renovate: datasource=github-tags depName=chriskohlhoff/asio
set(MINITUN_ASIO_VERSION "1.38.2")

function(minitun_setup_asio)
    if(MINITUN_USE_SYSTEM_DEPS)
        find_path(MINITUN_ASIO_INCLUDE_DIR
            NAMES asio.hpp
            DOC "Path containing standalone Asio headers"
        )
        if(NOT MINITUN_ASIO_INCLUDE_DIR)
            message(FATAL_ERROR
                "Standalone Asio was not found. Install libasio-dev/asio-devel "
                "or configure with -DMINITUN_USE_SYSTEM_DEPS=OFF."
            )
        endif()
    else()
        FetchContent_Declare(asio
            URL "https://downloads.sourceforge.net/project/asio/asio/${MINITUN_ASIO_VERSION}%20%28Stable%29/asio-${MINITUN_ASIO_VERSION}.tar.bz2"
            URL_HASH "SHA256=c04e0e66ac29741faad763a56f3c50196421d4b968009fc237c53314769bf8ad"
        )
        FetchContent_MakeAvailable(asio)
        set(MINITUN_ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/include")
    endif()

    add_library(minitun_asio INTERFACE)
    add_library(MiniTun::asio ALIAS minitun_asio)
    target_include_directories(minitun_asio SYSTEM INTERFACE
        "${MINITUN_ASIO_INCLUDE_DIR}"
    )
    target_compile_definitions(minitun_asio INTERFACE ASIO_STANDALONE)
    target_link_libraries(minitun_asio INTERFACE Threads::Threads)
endfunction()

function(minitun_setup_dependencies)
    find_package(Threads REQUIRED)
    find_package(OpenSSL 3.0 REQUIRED)
    find_package(SQLite3 REQUIRED)

    if(MINITUN_USE_SYSTEM_DEPS)
        find_package(CLI11 CONFIG REQUIRED)
        find_package(nlohmann_json CONFIG REQUIRED)
        find_package(spdlog CONFIG REQUIRED)
        if(MINITUN_BUILD_TESTS)
            find_package(GTest CONFIG REQUIRED)
        endif()
    else()
        set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(cli11
            URL "https://github.com/CLIUtils/CLI11/archive/refs/tags/v${MINITUN_CLI11_VERSION}.tar.gz"
            URL_HASH "SHA256=7828464fabc29d361e1ccb2d8721e6e00e73ff5c5fad5135408a8c236508398a"
        )

        set(JSON_BuildTests OFF CACHE INTERNAL "")
        set(JSON_Install OFF CACHE INTERNAL "")
        FetchContent_Declare(nlohmann_json
            URL "https://github.com/nlohmann/json/archive/refs/tags/v${MINITUN_NLOHMANN_JSON_VERSION}.tar.gz"
            URL_HASH "SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187"
        )

        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(spdlog
            URL "https://github.com/gabime/spdlog/archive/refs/tags/v${MINITUN_SPDLOG_VERSION}.tar.gz"
            URL_HASH "SHA256=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744"
        )

        FetchContent_MakeAvailable(cli11 nlohmann_json spdlog)

        if(MINITUN_BUILD_TESTS)
            set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
            set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(googletest
                URL "https://github.com/google/googletest/archive/refs/tags/v${MINITUN_GTEST_VERSION}.tar.gz"
                URL_HASH "SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c"
            )
            FetchContent_MakeAvailable(googletest)
        endif()
    endif()

    minitun_setup_asio()

    if(NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
        message(FATAL_ERROR "The OpenSSL package did not provide its CMake targets")
    endif()
    if(TARGET SQLite3::SQLite3)
        set(minitun_sqlite_target SQLite3::SQLite3)
    elseif(TARGET SQLite::SQLite3)
        set(minitun_sqlite_target SQLite::SQLite3)
    else()
        message(FATAL_ERROR "The SQLite3 package did not provide a CMake target")
    endif()

    add_library(minitun_sqlite INTERFACE)
    add_library(MiniTun::sqlite ALIAS minitun_sqlite)
    target_link_libraries(minitun_sqlite INTERFACE "${minitun_sqlite_target}")

    add_library(minitun_dependencies INTERFACE)
    add_library(MiniTun::dependencies ALIAS minitun_dependencies)
    target_link_libraries(minitun_dependencies
        INTERFACE
            MiniTun::asio
            OpenSSL::SSL
            OpenSSL::Crypto
            nlohmann_json::nlohmann_json
    )
endfunction()
