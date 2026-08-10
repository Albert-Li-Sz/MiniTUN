if(NOT DEFINED MINITUN_LIBRARY OR NOT EXISTS "${MINITUN_LIBRARY}")
    message(FATAL_ERROR "MINITUN_LIBRARY does not name the built SDK library")
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
set(actual_symbols)
set(unexpected_symbols)
foreach(line IN LISTS nm_lines)
    string(STRIP "${line}" line)
    if(line MATCHES "([_A-Za-z][_A-Za-z0-9@.]*)$")
        set(symbol "${CMAKE_MATCH_1}")
        if(APPLE AND symbol MATCHES "^_minitun_")
            string(SUBSTRING "${symbol}" 1 -1 symbol)
            list(APPEND actual_symbols "${symbol}")
        elseif(NOT APPLE AND symbol MATCHES "^minitun_")
            string(REGEX REPLACE "@.*$" "" symbol "${symbol}")
            list(APPEND actual_symbols "${symbol}")
        elseif(NOT symbol STREQUAL "MINITUN_CLIENT_1.0")
            list(APPEND unexpected_symbols "${symbol}")
        endif()
    endif()
endforeach()
list(REMOVE_DUPLICATES actual_symbols)
list(SORT actual_symbols)
list(REMOVE_DUPLICATES unexpected_symbols)

file(STRINGS "${MINITUN_BASELINE}" expected_symbols)
list(FILTER expected_symbols EXCLUDE REGEX "^[ \t]*(#|$)")
list(SORT expected_symbols)

if(unexpected_symbols)
    message(FATAL_ERROR "SDK exports non-ABI symbols: ${unexpected_symbols}")
endif()
if(NOT actual_symbols STREQUAL expected_symbols)
    message(FATAL_ERROR
        "SDK ABI differs from baseline\nExpected: ${expected_symbols}\nActual: ${actual_symbols}")
endif()

message(STATUS "MiniTun Client ABI baseline matches")
