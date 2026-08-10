cmake_minimum_required(VERSION 3.22)
project(MiniTunDownstream LANGUAGES C)

find_package(MiniTun 1.0 REQUIRED CONFIG)

add_executable(minitun_downstream main.c)
target_link_libraries(minitun_downstream PRIVATE MiniTun::Client)
set_target_properties(minitun_downstream PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
