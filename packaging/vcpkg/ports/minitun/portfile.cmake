# Pin the source by release tag instead of downloading an archive with a fixed
# SHA512. vcpkg_from_github needs a real SHA512 for the release tarball, which
# only exists after the first tagged release is cut; cloning by tag keeps this
# overlay usable before then and stays deterministic once tags are immutable.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/Albert-Li-Sz/MiniTUN.git
    REF "v${VERSION}"
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMINITUN_USE_SYSTEM_DEPS=OFF
        -DMINITUN_BUILD_TESTS=OFF
        -DMINITUN_BUILD_PACKAGES=OFF
        -DMINITUN_BUILD_FUZZERS=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/MiniTun)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
