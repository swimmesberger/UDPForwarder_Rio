cmake_minimum_required(VERSION 3.21)

project(UDPForwarder_Rio C)

set(CMAKE_C_STANDARD 11)

add_definitions(-DRIOSOCKETS_DLL)
add_executable(UDPForwarder_Rio
        main.c
        deps/RioSockets/Source/riosockets.c
        deps/Argtable3/argtable3.c
)

if (NOT UNIX)
    target_link_libraries(UDPForwarder_Rio PRIVATE ws2_32)
    SET_TARGET_PROPERTIES(UDPForwarder_Rio PROPERTIES PREFIX "")
endif()