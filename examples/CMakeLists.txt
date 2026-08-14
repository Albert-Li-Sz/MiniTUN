cmake_minimum_required(VERSION 3.22)
project(minitun_examples LANGUAGES CXX)

# 示例：以已安装的 MiniTun SDK 构建。
#   cmake -S examples -B build-examples -DCMAKE_PREFIX_PATH=/usr
find_package(MiniTun 1.0 REQUIRED)

add_executable(local_control local_control.cpp)
target_link_libraries(local_control PRIVATE MiniTun::Client)

add_executable(remote_codec remote_codec.cpp)
target_link_libraries(remote_codec PRIVATE MiniTun::RemoteProtocol)
