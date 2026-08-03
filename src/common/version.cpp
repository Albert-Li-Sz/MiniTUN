#include <minitun/common/version.hpp>

#include <limits>
#include <sstream>

#ifndef MINITUN_VERSION
#define MINITUN_VERSION "0.2.3-dev"
#endif

#ifndef MINITUN_GIT_COMMIT
#define MINITUN_GIT_COMMIT "unknown"
#endif

#ifndef MINITUN_BUILD_TYPE
#ifdef NDEBUG
#define MINITUN_BUILD_TYPE "Release"
#else
#define MINITUN_BUILD_TYPE "Debug"
#endif
#endif

#ifndef MINITUN_COMPILER
#if defined(__clang__)
#define MINITUN_COMPILER "Clang " __clang_version__
#elif defined(__GNUC__)
#define MINITUN_COMPILER "GCC " __VERSION__
#elif defined(_MSC_VER)
#define MINITUN_STRINGIFY_IMPL(value) #value
#define MINITUN_STRINGIFY(value) MINITUN_STRINGIFY_IMPL(value)
#define MINITUN_COMPILER "MSVC " MINITUN_STRINGIFY(_MSC_VER)
#else
#define MINITUN_COMPILER "unknown"
#endif
#endif

#ifndef MINITUN_PROTOCOL_VERSION
#define MINITUN_PROTOCOL_VERSION 1
#endif

namespace minitun::common {
namespace {

static_assert(MINITUN_PROTOCOL_VERSION >= 0, "MINITUN_PROTOCOL_VERSION must be non-negative");
static_assert(static_cast<unsigned long>(MINITUN_PROTOCOL_VERSION) <=
                  static_cast<unsigned long>(std::numeric_limits<std::uint16_t>::max()),
              "MINITUN_PROTOCOL_VERSION must fit in uint16_t");

} // namespace

VersionInfo version_info() noexcept {
    return VersionInfo{
        .version = MINITUN_VERSION,
        .git_commit = MINITUN_GIT_COMMIT,
        .build_type = MINITUN_BUILD_TYPE,
        .compiler = MINITUN_COMPILER,
        .protocol_version = static_cast<std::uint16_t>(MINITUN_PROTOCOL_VERSION),
    };
}

std::string format_version_info(const std::string_view program_name) {
    const VersionInfo info = version_info();

    std::ostringstream output;
    output << program_name << ' ' << info.version << '\n'
           << "git commit: " << info.git_commit << '\n'
           << "build type: " << info.build_type << '\n'
           << "compiler: " << info.compiler << '\n'
           << "protocol version: " << info.protocol_version;
    return output.str();
}

std::string version_text() { return format_version_info("MiniTun"); }

} // namespace minitun::common
