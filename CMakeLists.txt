cmake_minimum_required(VERSION 3.20)
project(wavedb VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# ── 源文件 ──────────────────────────────────
set(LIB_SOURCES
  src/catalog/schema.cpp
  src/catalog/catalog.cpp
  src/storage/column_file.cpp
  src/storage/part.cpp
  src/storage/part_manager.cpp
  src/engine/wavedb.cpp
  src/engine/connection.cpp
  src/engine/appender.cpp
  src/common/time_format.cpp
  src/parser/parser.cpp
  src/storage/merge_scheduler.cpp
)

set(LIB_HEADERS
  include/wavedb.h
  src/common/types.h
  src/common/status.h
  src/catalog/schema.h
  src/engine/wavedb.h
  src/engine/connection.h
  src/engine/appender.h
)

# ── 对象库（编译一次，.a 和 .so 共享 .o）───────
add_library(wavedb_objs OBJECT ${LIB_SOURCES})
target_include_directories(wavedb_objs PUBLIC
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)
set_target_properties(wavedb_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── 静态库 ──────────────────────────────────
add_library(wavedb_static STATIC)
target_link_libraries(wavedb_static wavedb_objs)
target_include_directories(wavedb_static PUBLIC
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)
set_target_properties(wavedb_static PROPERTIES OUTPUT_NAME wavedb)

# ── 动态库 ──────────────────────────────────
add_library(wavedb_shared SHARED)
target_link_libraries(wavedb_shared wavedb_objs)
target_include_directories(wavedb_shared PUBLIC
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)
set_target_properties(wavedb_shared PROPERTIES OUTPUT_NAME wavedb)

# ── CLI 源文件 ───────────────────────────────
set(CLI_SOURCES
  src/main.cpp
  src/cli/cli.cpp
)

# ── CLI 可执行文件 ───────────────────────────
# linenoise.c 直接编入 CLI（单文件 C 源码，零外部依赖）
set_source_files_properties(third_party/linenoise.c PROPERTIES LANGUAGE C)
add_executable(wavedb ${CLI_SOURCES} third_party/linenoise.c)
target_include_directories(wavedb PRIVATE third_party)
target_link_libraries(wavedb wavedb_static)

# ── 单元测试 ────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
  add_subdirectory(third_party/googletest ${CMAKE_BINARY_DIR}/googletest
                   EXCLUDE_FROM_ALL)

  enable_testing()

  function(add_wavedb_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} wavedb_static gtest_main gmock_main)
    include(GoogleTest)
    gtest_discover_tests(${name})
  endfunction()

  add_wavedb_test(test_common)
  add_wavedb_test(test_storage)
  add_wavedb_test(test_catalog)
  add_wavedb_test(test_engine)
endif()

# ── 安装（放最后，避免 GTest 污染）───────────
install(TARGETS wavedb_static wavedb_shared
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(TARGETS wavedb
  RUNTIME DESTINATION bin
)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/wavedb/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wavedb
  FILES_MATCHING PATTERN "*.h"
)
