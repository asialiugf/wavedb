cmake_minimum_required(VERSION 3.20)
project(wavedb_writer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

add_executable(writer writer.cpp)

target_include_directories(writer PRIVATE
  ${CMAKE_SOURCE_DIR}/../../include
  ${CMAKE_SOURCE_DIR}/../..
)

target_link_directories(writer PRIVATE ${CMAKE_SOURCE_DIR}/../../lib)
target_link_libraries(writer wavedb pthread)
