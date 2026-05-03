cmake_minimum_required(VERSION 3.20)
project(wavedb VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# 共享源文件编译为静态库
add_library(wavedb_lib STATIC
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

target_include_directories(wavedb_lib PUBLIC ${CMAKE_SOURCE_DIR})

# 主程序
add_executable(wavedb src/main.cpp)
target_link_libraries(wavedb wavedb_lib)

# 工具
add_executable(writer tools/writer.cpp)
target_link_libraries(writer wavedb_lib)

add_executable(reader tools/reader.cpp)
target_link_libraries(reader wavedb_lib)

# 单元测试
enable_testing()

function(add_wavedb_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} wavedb_lib gtest_main gmock_main)
  include(GoogleTest)
  gtest_discover_tests(${name})
endfunction()

add_wavedb_test(test_common)
add_wavedb_test(test_storage)
add_wavedb_test(test_catalog)
add_wavedb_test(test_engine)
