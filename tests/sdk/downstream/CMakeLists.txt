cmake_minimum_required(VERSION 3.22)
project(MiniTunDownstream LANGUAGES C CXX)

find_package(MiniTun 1.0 REQUIRED CONFIG)

add_executable(minitun_downstream main.c)
target_link_libraries(minitun_downstream PRIVATE MiniTun::Client)
set_target_properties(minitun_downstream PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)

add_executable(minitun_remote_downstream remote.cpp)
target_link_libraries(minitun_remote_downstream PRIVATE MiniTun::RemoteProtocol)
set_target_properties(minitun_remote_downstream PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
