cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(VirtualOffice VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(Boost_USE_STATIC_LIBS ON)

if(MSVC)
  set (Boost_USE_STATIC_RUNTIME ON)
  add_definitions(-D_WIN32_WINNT=0x0601)
  add_definitions(-D_SCL_SECURE_NO_WARNINGS=1)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS=1)
  add_definitions(-D_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING)
  add_compile_options(/MP)
  string(REPLACE "/W3" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER build)

find_package(Threads REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

include_directories(${Boost_INCLUDE_DIRS})
link_directories(${Boost_LIBRARY_DIRS})

file(GLOB_RECURSE BEAST_FILES
  ${Boost_INCLUDE_DIRS}/boost/beast/*.hpp
  ${Boost_INCLUDE_DIRS}/boost/beast/*.ipp)

source_group(TREE ${Boost_INCLUDE_DIRS}/boost/beast PREFIX beast FILES ${BEAST_FILES})

add_executable(http_echo 
  net/net.h
  http_echo.cpp
  ${BEAST_FILES}
)
target_link_libraries(http_echo PRIVATE Threads::Threads ${Boost_SYSTEM_LIBRARY})

add_executable(websocket_echo 
  net/net.h
  websocket_echo.cpp
  ${BEAST_FILES}
)
target_link_libraries(websocket_echo PRIVATE Threads::Threads ${Boost_SYSTEM_LIBRARY})

add_executable(websocket_broadcast 
  net/net.h
  websocket_broadcast.cpp
  ${BEAST_FILES}
)
target_link_libraries(websocket_broadcast PRIVATE Threads::Threads ${Boost_SYSTEM_LIBRARY})