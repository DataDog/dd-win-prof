cmake_minimum_required(VERSION 3.21)
project(dd_win_prof_downstream LANGUAGES CXX)

# Uses dd-win-prof only through the target exported by ../CMakeLists.txt and installed
# to DD_WIN_PROF_CONSUMER_PREFIX (see scripts/test-consumer.ps1): the exported target
# must carry its include path and import library, and the header must be installed.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DD_WIN_PROF_CONSUMER_PREFIX)
    message(FATAL_ERROR "Pass -DDD_WIN_PROF_CONSUMER_PREFIX=<install prefix of the consumer test>")
endif()

include("${DD_WIN_PROF_CONSUMER_PREFIX}/lib/cmake/consumer/ConsumerTargets.cmake")

add_executable(downstream ../main.cpp)
target_link_libraries(downstream PRIVATE dd-win-prof)
