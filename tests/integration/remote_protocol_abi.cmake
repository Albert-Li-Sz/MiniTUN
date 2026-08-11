if(NOT DEFINED MINITUN_LIBRARY OR NOT EXISTS "${MINITUN_LIBRARY}")
    message(FATAL_ERROR "MINITUN_LIBRARY does not name the Remote Protocol SDK")
endif()
if(NOT DEFINED MINITUN_NM OR NOT EXISTS "${MINITUN_NM}")
    message(FATAL_ERROR "MINITUN_NM does not name nm")
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
list(LENGTH remote_symbols remote_symbol_count)

if(unexpected_symbols)
    message(FATAL_ERROR
        "Remote Protocol SDK exports implementation symbols: ${unexpected_symbols}")
endif()
if(NOT remote_symbol_count EQUAL 15)
    message(FATAL_ERROR
        "Remote Protocol SDK exports ${remote_symbol_count} API symbols; expected 15")
endif()

message(STATUS "MiniTun Remote Protocol SDK export boundary matches")
