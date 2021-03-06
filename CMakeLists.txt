CMAKE_MINIMUM_REQUIRED(VERSION 2.8)
PROJECT(COCAINE-NATIVE-PROXY)

SET(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

FIND_PACKAGE(Boost 1.40.0 REQUIRED COMPONENTS system thread)

INCLUDE(cmake/locate_library.cmake)

LOCATE_LIBRARY(LIBFRAMEWORK "cocaine/framework/service.hpp" "cocaine-framework")
LOCATE_LIBRARY(LIBSWARM "swarm/http_response.hpp" "swarm")
LOCATE_LIBRARY(LIBTHEVOID "thevoid/server.hpp" "thevoid")
LOCATE_LIBRARY(LIBEV "ev++.h" "ev" "libev")
LOCATE_LIBRARY(LIBMSGPACK "msgpack.hpp" "msgpack")

INCLUDE_DIRECTORIES(
    ${Boost_INCLUDE_DIRS}
    ${LIBEV_INCLUDE_DIRS}
    ${LIBMSGPACK_INCLUDE_DIRS}
    ${LIBFRAMEWORK_INCLUDE_DIRS}
    ${LIBSWARM_INCLUDE_DIRS}
    ${LIBTHEVOID_INCLUDE_DIRS})

LINK_DIRECTORIES(
    ${Boost_LIBRARY_DIRS}
    ${LIBEV_LIBRARY_DIRS}
    ${LIBMSGPACK_LIBRARY_DIRS}
    ${LIBFRAMEWORK_LIBRARY_DIRS}
    ${LIBSWARM_LIBRARY_DIRS}
    ${LIBTHEVOID_LIBRARY_DIRS})

ADD_EXECUTABLE(cocaine-native-proxy
    proxy)

TARGET_LINK_LIBRARIES(cocaine-native-proxy
    cocaine-framework
    cocaine-core
    swarm
    thevoid
    ev
    msgpack
    boost_system
    boost_thread
    -pthread)

SET_TARGET_PROPERTIES(cocaine-native-proxy PROPERTIES
    COMPILE_FLAGS "-std=c++0x -W -Wall -Werror -pedantic")

INSTALL(
    TARGETS
        cocaine-native-proxy
    RUNTIME DESTINATION bin COMPONENT runtime)
