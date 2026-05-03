cmake_minimum_required(VERSION 3.20)
project(wavedb_reader LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

add_executable(reader reader.cpp)

target_include_directories(reader PRIVATE
  ${CMAKE_SOURCE_DIR}/../../include
  ${CMAKE_SOURCE_DIR}/../..
)

target_link_directories(reader PRIVATE ${CMAKE_SOURCE_DIR}/../../lib)
target_link_libraries(reader wavedb pthread)
