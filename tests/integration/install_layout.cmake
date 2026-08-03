cmake_minimum_required(VERSION 3.22)

foreach(variable
        MINITUN_BUILD_DIR
        MINITUN_BINDIR
        MINITUN_LIBEXECDIR
        MINITUN_INCLUDEDIR
        MINITUN_MANDIR
        MINITUN_SYSCONFDIR
        MINITUN_SYSTEMD_UNIT_DIR
        MINITUN_SYSUSERS_DIR)
    if(NOT DEFINED "${variable}")
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

function(install_component component output_variable)
    set(prefix "${MINITUN_BUILD_DIR}/install-smoke/${component}")
    file(REMOVE_RECURSE "${prefix}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${prefix}"
                "${CMAKE_COMMAND}" --install "${MINITUN_BUILD_DIR}"
                --prefix /usr --component "${component}"
        RESULT_VARIABLE install_result
        OUTPUT_VARIABLE install_output
        ERROR_VARIABLE install_error
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR
            "${component} installation failed:\n${install_output}\n${install_error}")
    endif()
    set("${output_variable}" "${prefix}" PARENT_SCOPE)
endfunction()

function(staged_path install_directory output_variable)
    if(IS_ABSOLUTE "${install_directory}")
        string(REGEX REPLACE "^/+" "" relative_path "${install_directory}")
    else()
        set(relative_path "usr/${install_directory}")
    endif()
    set("${output_variable}" "${relative_path}" PARENT_SCOPE)
endfunction()

function(require_paths root)
    foreach(relative_path IN LISTS ARGN)
        if(NOT EXISTS "${root}/${relative_path}")
            message(FATAL_ERROR "installed path is missing: ${relative_path}")
        endif()
    endforeach()
endfunction()

function(run_version executable expected_name)
    execute_process(
        COMMAND "${executable}" --version
        RESULT_VARIABLE version_result
        OUTPUT_VARIABLE version_output
        ERROR_VARIABLE version_error
    )
    if(NOT version_result EQUAL 0 OR
       NOT version_output MATCHES "${expected_name} [0-9]+\\.[0-9]+\\.[0-9]+")
        message(FATAL_ERROR
            "installed ${expected_name} version check failed:\n${version_output}\n${version_error}")
    endif()
endfunction()

staged_path("${MINITUN_BINDIR}" staged_bindir)
staged_path("${MINITUN_LIBEXECDIR}" staged_libexecdir)
staged_path("${MINITUN_INCLUDEDIR}" staged_includedir)
staged_path("${MINITUN_MANDIR}" staged_mandir)
staged_path("${MINITUN_SYSCONFDIR}" staged_sysconfdir)
staged_path("${MINITUN_SYSTEMD_UNIT_DIR}" staged_systemd_unit_dir)
staged_path("${MINITUN_SYSUSERS_DIR}" staged_sysusers_dir)

install_component(Client client_prefix)
require_paths("${client_prefix}"
    "${staged_bindir}/minitun"
    "${staged_libexecdir}/minitun/minitund"
    "${staged_systemd_unit_dir}/minitund.service"
    "${staged_sysusers_dir}/minitun.conf"
    "${staged_mandir}/man1/minitun.1"
    "${staged_mandir}/man8/minitund.8"
)
if(EXISTS "${client_prefix}/${staged_bindir}/minitun-server")
    message(FATAL_ERROR "Client component unexpectedly installed minitun-server")
endif()
execute_process(
    COMMAND "${client_prefix}/${staged_bindir}/minitun" version
    RESULT_VARIABLE client_version_result
    OUTPUT_VARIABLE client_version_output
)
if(NOT client_version_result EQUAL 0 OR NOT client_version_output MATCHES "minitun [0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "installed minitun version check failed")
endif()
run_version("${client_prefix}/${staged_libexecdir}/minitun/minitund" "minitund")

install_component(Server server_prefix)
require_paths("${server_prefix}"
    "${staged_bindir}/minitun-server"
    "${staged_systemd_unit_dir}/minitun-server.service"
    "${staged_sysusers_dir}/minitun-server.conf"
    "${staged_mandir}/man8/minitun-server.8"
    "${staged_sysconfdir}/minitun-server/README"
)
if(EXISTS "${server_prefix}/${staged_bindir}/minitun")
    message(FATAL_ERROR "Server component unexpectedly installed minitun")
endif()
run_version("${server_prefix}/${staged_bindir}/minitun-server" "minitun-server")
file(READ "${server_prefix}/${staged_systemd_unit_dir}/minitun-server.service"
     server_service)
if(NOT server_service MATCHES "--token-file /etc/minitun-server/token")
    message(FATAL_ERROR "server service does not reference the required Token file")
endif()
if(server_service MATCHES "--allow-ports")
    message(FATAL_ERROR "server service unexpectedly restricts the default tunnel port range")
endif()
if(NOT server_service MATCHES "AmbientCapabilities=CAP_NET_BIND_SERVICE" OR
   NOT server_service MATCHES "CapabilityBoundingSet=CAP_NET_BIND_SERVICE")
    message(FATAL_ERROR "server service cannot bind the full configured TCP port range")
endif()

install_component(Development development_prefix)
require_paths("${development_prefix}"
    "${staged_includedir}/minitun/common/result.hpp"
    "${staged_includedir}/minitun/protocol/frame.hpp"
)
if(EXISTS "${development_prefix}/${staged_bindir}/minitun")
    message(FATAL_ERROR "Development component unexpectedly installed runtime binaries")
endif()

message(STATUS "component installation layout passed")
