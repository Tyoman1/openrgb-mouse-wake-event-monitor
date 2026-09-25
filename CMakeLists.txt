cmake_minimum_required(VERSION 3.21)
project(WakeEventMonitor LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(WakeEventMonitor src/WakeEventMonitor.cpp)
target_link_libraries(WakeEventMonitor PRIVATE setupapi.lib hid.lib user32.lib)
target_compile_definitions(WakeEventMonitor PRIVATE UNICODE _UNICODE)