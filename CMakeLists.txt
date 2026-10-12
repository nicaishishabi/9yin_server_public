cmake_minimum_required(VERSION 3.16)
project(nineyin_cpp_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/utf-8 /W3)
endif()

add_library(nineyin_core STATIC
  src/transport/key.cpp
  src/transport/frame.cpp
  src/transport/connection.cpp
  src/config/paths.cpp
  src/util/log.cpp
  src/gm/hub.cpp
  src/gm/http_server.cpp
  src/db/role_store.cpp
  src/scene/path_file.cpp
  src/scene/npc_scene.cpp
  src/scene/npc_template.cpp
  src/game/systems.cpp
)
target_include_directories(nineyin_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Optional MySQL client
set(NINEYIN_HAS_MYSQL OFF)
find_path(MYSQL_INCLUDE_DIR mysql.h
  PATHS
    ENV MYSQL_DIR
    "C:/Program Files/MySQL/MySQL Server 8.0/include"
    "C:/Program Files/MySQL/MySQL Server 8.4/include"
    "${CMAKE_SOURCE_DIR}/mysql-server/mysql-26.7.0-winx64/include"
  PATH_SUFFIXES mysql
)
find_library(MYSQL_LIBRARY
  NAMES libmysql libmysqlclient mysqlclient
  PATHS
    ENV MYSQL_DIR
    "C:/Program Files/MySQL/MySQL Server 8.0/lib"
    "C:/Program Files/MySQL/MySQL Server 8.4/lib"
    "${CMAKE_SOURCE_DIR}/mysql-server/mysql-26.7.0-winx64/lib"
)
if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
  set(NINEYIN_HAS_MYSQL ON)
  target_include_directories(nineyin_core PRIVATE ${MYSQL_INCLUDE_DIR})
  target_link_libraries(nineyin_core PUBLIC ${MYSQL_LIBRARY})
  target_compile_definitions(nineyin_core PUBLIC NINEYIN_HAS_MYSQL=1)
  message(STATUS "MySQL client found: ${MYSQL_LIBRARY}")
else()
  message(WARNING "MySQL client not found; building without MySQL support")
endif()

if(WIN32)
  target_link_libraries(nineyin_core PUBLIC ws2_32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(nineyin_core PUBLIC Threads::Threads)

add_executable(protocol_probe src/main.cpp)
target_include_directories(protocol_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(protocol_probe PRIVATE nineyin_core)
