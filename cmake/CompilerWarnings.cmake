add_library(minitun_project_warnings INTERFACE)

target_compile_options(minitun_project_warnings
    INTERFACE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>"
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>"
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wpedantic>"
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wconversion>"
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wshadow>"
        "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
        "$<$<AND:$<BOOL:${MINITUN_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>"
        "$<$<AND:$<BOOL:${MINITUN_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:MSVC>>:/WX>"
)
