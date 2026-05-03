cmake_minimum_required(VERSION 3.20)
project(wavedb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(wavedb
  src/main.cpp
  src/catalog/schema.cpp
  src/catalog/catalog.cpp
  src/storage/column_file.cpp
  src/storage/part.cpp
  src/storage/part_manager.cpp
  src/engine/wavedb.cpp
  src/engine/connection.cpp
  src/engine/appender.cpp
  src/common/time_format.cpp
)

target_include_directories(wavedb PRIVATE ${CMAKE_SOURCE_DIR})
