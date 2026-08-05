if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(WARNING "MiniTun DEB and RPM packages are intended for Linux targets")
endif()

set(CPACK_PACKAGE_NAME "minitun")
set(CPACK_PACKAGE_VENDOR "MiniTun")
set(CPACK_PACKAGE_CONTACT "MiniTun maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Small, high-performance TCP reverse-tunnelling system"
)
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(minitun_debian_version "${MINITUN_PACKAGE_VERSION}")
set(minitun_rpm_version "${MINITUN_PACKAGE_VERSION}")
set(minitun_rpm_release 1)
if(MINITUN_PACKAGE_VERSION MATCHES
   "^([0-9]+\\.[0-9]+\\.[0-9]+)-rc\\.([0-9]+)$")
    set(minitun_debian_version "${CMAKE_MATCH_1}~rc.${CMAKE_MATCH_2}")
    set(minitun_rpm_version "${CMAKE_MATCH_1}")
    set(minitun_rpm_release "0.rc.${CMAKE_MATCH_2}")
elseif(MINITUN_PACKAGE_VERSION MATCHES
       "^([0-9]+\\.[0-9]+\\.[0-9]+)_pre([0-9]+)~([0-9a-f]+)$")
    set(minitun_debian_version
        "${CMAKE_MATCH_1}~pre.${CMAKE_MATCH_2}+git.${CMAKE_MATCH_3}"
    )
    set(minitun_rpm_version "${CMAKE_MATCH_1}")
    set(minitun_rpm_release "0.pre.${CMAKE_MATCH_2}.git.${CMAKE_MATCH_3}")
endif()

set(CPACK_PACKAGE_VERSION "${MINITUN_PACKAGE_VERSION}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGE_RELOCATABLE OFF)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_SET_DESTDIR ON)
set(CPACK_MONOLITHIC_INSTALL OFF)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_COMPONENTS_ALL Client Server)

set(CPACK_COMPONENT_CLIENT_DISPLAY_NAME "MiniTun Client")
set(CPACK_COMPONENT_CLIENT_DESCRIPTION
    "CLI and local daemon for connecting to MiniTun public servers"
)
set(CPACK_COMPONENT_SERVER_DISPLAY_NAME "MiniTun Server")
set(CPACK_COMPONENT_SERVER_DESCRIPTION
    "Public TLS server for MiniTun reverse TCP tunnels"
)

# Explicit architecture overrides for cross-compiled packages. Leave empty to
# let CPack infer the architecture from CMAKE_SYSTEM_PROCESSOR (native builds).
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "" CACHE STRING
    "Explicit Debian package architecture (e.g. arm64, armhf, riscv64)"
)
set(CPACK_RPM_PACKAGE_ARCHITECTURE "" CACHE STRING
    "Explicit RPM package architecture (e.g. aarch64, armv7hl, riscv64)"
)

set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "MiniTun maintainers")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${PROJECT_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
set(CPACK_DEBIAN_PACKAGE_VERSION "${minitun_debian_version}")

set(CPACK_DEBIAN_CLIENT_PACKAGE_NAME "minitun-client")
set(CPACK_DEBIAN_CLIENT_PACKAGE_SECTION net)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON CACHE BOOL
    "Derive DEB dependencies with dpkg-shlibdeps"
)
set(CPACK_DEBIAN_CLIENT_PACKAGE_DEPENDS systemd CACHE STRING
    "Runtime dependencies for the minitun-client DEB"
)
set(CPACK_DEBIAN_CLIENT_PACKAGE_CONTROL_EXTRA
    "${PROJECT_SOURCE_DIR}/packaging/debian/postinst"
    "${PROJECT_SOURCE_DIR}/packaging/debian/client/postrm"
)

set(CPACK_DEBIAN_SERVER_PACKAGE_NAME "minitun-server")
set(CPACK_DEBIAN_SERVER_PACKAGE_SECTION net)
set(CPACK_DEBIAN_SERVER_PACKAGE_DEPENDS systemd CACHE STRING
    "Runtime dependencies for the minitun-server DEB"
)
set(CPACK_DEBIAN_SERVER_PACKAGE_CONTROL_EXTRA
    "${PROJECT_SOURCE_DIR}/packaging/debian/postinst"
    "${PROJECT_SOURCE_DIR}/packaging/debian/server/postrm"
    "${PROJECT_SOURCE_DIR}/packaging/debian/server/conffiles"
)

set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE MIT)
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_VERSION "${minitun_rpm_version}")
set(CPACK_RPM_PACKAGE_RELEASE "${minitun_rpm_release}")
set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)
set(CPACK_RPM_PACKAGE_AUTOREQ ON CACHE BOOL
    "Derive RPM requirements from ELF dependencies"
)

set(CPACK_RPM_CLIENT_PACKAGE_NAME "minitun-client")
set(CPACK_RPM_CLIENT_PACKAGE_REQUIRES systemd CACHE STRING
    "Runtime dependencies for the minitun-client RPM"
)
set(CPACK_RPM_CLIENT_POST_INSTALL_SCRIPT_FILE
    "${PROJECT_SOURCE_DIR}/packaging/rpm/post-install.sh"
)
set(CPACK_RPM_CLIENT_POST_UNINSTALL_SCRIPT_FILE
    "${PROJECT_SOURCE_DIR}/packaging/rpm/post-uninstall.sh"
)

set(CPACK_RPM_SERVER_PACKAGE_NAME "minitun-server")
set(CPACK_RPM_SERVER_PACKAGE_REQUIRES systemd CACHE STRING
    "Runtime dependencies for the minitun-server RPM"
)
set(CPACK_RPM_SERVER_POST_INSTALL_SCRIPT_FILE
    "${PROJECT_SOURCE_DIR}/packaging/rpm/post-install.sh"
)
set(CPACK_RPM_SERVER_POST_UNINSTALL_SCRIPT_FILE
    "${PROJECT_SOURCE_DIR}/packaging/rpm/post-uninstall.sh"
)
set(CPACK_RPM_SERVER_USER_FILELIST
    "%config(noreplace) /etc/minitun-server/README"
)

include(CPack)
