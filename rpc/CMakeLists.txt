cmake_minimum_required(VERSION 3.7)
find_package(Boost REQUIRED)
find_package(folly REQUIRED)
find_package(Threads REQUIRED)
find_package(gflags REQUIRED)

enable_testing()
find_package(GTest REQUIRED)

set(CMAKE_CXX_STANDARD 14)
#include_directories(${Boost_INCLUDE_DIRS})
#include_directories(${folly_INCLUDE_DIRS})

set_and_check(FOLLY_INCLUDE_DIR /usr/local/include/folly)
set_and_check(FOLLY_CMAKE_DIR /usr/local/lib/cmake/folly)
if (NOT TARGET Folly::folly)
    include("${FOLLY_CMAKE_DIR}/folly-targets.cmake")
endif()


set(FOLLY_LIBRARIES Folly::folly)

if (NOT folly_FIND_QUIETLY)
    message(STATUS "Found folly: ${PACKAGE_PREFIX_DIR}")
endif()

add_executable(server epoll_future.cc)
target_link_libraries(server ${Boost_LIBRARIES} ${FOLLY_LIBRARIES} GTest::GTest GTest::Main)