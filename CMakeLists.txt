cmake_minimum_required(VERSION 3.10)

project(winsock-rdt)

set(CMAKE_CXX_STANDARD 20)
file(GLOB_RECURSE SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(endpoint ${SRC_FILES})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
    target_link_libraries(endpoint ws2_32)
else()
    # target_link_libraries(endpoint pthread)
endif()