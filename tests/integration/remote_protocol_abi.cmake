if(NOT DEFINED MINITUN_LIBRARY OR NOT EXISTS "${MINITUN_LIBRARY}")
    message(FATAL_ERROR "MINITUN_LIBRARY does not name the Remote Protocol SDK")
endif()
if(NOT DEFINED MINITUN_NM OR NOT EXISTS "${MINITUN_NM}")
    message(FATAL_ERROR "MINITUN_NM does not name nm")
endif()
if(NOT DEFINED MINITUN_BASELINE OR NOT EXISTS "${MINITUN_BASELINE}")
    message(FATAL_ERROR "MINITUN_BASELINE does not exist")
endif()

if(APPLE)
    execute_process(
        COMMAND "${MINITUN_NM}" -gU "${MINITUN_LIBRARY}"
        RESULT_VARIABLE nm_status
        OUTPUT_VARIABLE nm_output
        ERROR_VARIABLE nm_error
    )
else()
    execute_process(
        COMMAND "${MINITUN_NM}" -D --defined-only "${MINITUN_LIBRARY}"
        RESULT_VARIABLE nm_status
        OUTPUT_VARIABLE nm_output
        ERROR_VARIABLE nm_error
    )
endif()
if(NOT nm_status EQUAL 0)
    message(FATAL_ERROR "nm failed: ${nm_error}")
endif()

string(REPLACE "\r\n" "\n" nm_output "${nm_output}")
string(REPLACE "\n" ";" nm_lines "${nm_output}")
set(remote_symbols)
set(unexpected_symbols)
foreach(line IN LISTS nm_lines)
    string(STRIP "${line}" line)
    if(line MATCHES "([_A-Za-z][_A-Za-z0-9@.]*)$")
        set(symbol "${CMAKE_MATCH_1}")
        if(APPLE AND symbol MATCHES "^__ZNK?7minitun6remote")
            # Mach-O prefixes C++ symbols with an extra underscore; strip it so
            # the same baseline works on Linux and macOS.
            string(SUBSTRING "${symbol}" 1 -1 symbol)
            list(APPEND remote_symbols "${symbol}")
        elseif(NOT APPLE AND symbol MATCHES "^_ZNK?7minitun6remote")
            string(REGEX REPLACE "@.*$" "" symbol "${symbol}")
            list(APPEND remote_symbols "${symbol}")
        elseif(NOT symbol STREQUAL "MINITUN_REMOTE_PROTOCOL_1.0")
            list(APPEND unexpected_symbols "${symbol}")
        endif()
    endif()
endforeach()
list(REMOVE_DUPLICATES remote_symbols)
list(REMOVE_DUPLICATES unexpected_symbols)

# The baseline lists one mangled-name prefix per public API symbol. Matching by
# prefix keeps standard-library mangling differences out of the comparison
# while still failing when a method is added, removed, or re-signed, which is
# what an ABI break looks like for this C++20 surface.
file(STRINGS "${MINITUN_BASELINE}" baseline_prefixes)
list(FILTER baseline_prefixes EXCLUDE REGEX "^[ \t]*(#|$)")

set(matched_symbols)
set(unmatched_symbols ${remote_symbols})
foreach(prefix IN LISTS baseline_prefixes)
    set(matched_any FALSE)
    set(remaining_symbols)
    foreach(symbol IN LISTS unmatched_symbols)
        string(FIND "${symbol}" "${prefix}" prefix_offset)
        if(prefix_offset EQUAL 0)
            set(matched_any TRUE)
            list(APPEND matched_symbols "${symbol}")
        else()
            list(APPEND remaining_symbols "${symbol}")
        endif()
    endforeach()
    if(NOT matched_any)
        message(FATAL_ERROR
            "Remote Protocol SDK baseline prefix matches no exported symbol: ${prefix}")
    endif()
    set(unmatched_symbols ${remaining_symbols})
endforeach()

if(unexpected_symbols)
    message(FATAL_ERROR
        "Remote Protocol SDK exports implementation symbols: ${unexpected_symbols}")
endif()
if(unmatched_symbols)
    message(FATAL_ERROR
        "Remote Protocol SDK exports symbols absent from the baseline: ${unmatched_symbols}")
endif()

list(LENGTH remote_symbols remote_symbol_count)
message(STATUS
    "MiniTun Remote Protocol SDK export boundary matches baseline (${remote_symbol_count} symbols)")
