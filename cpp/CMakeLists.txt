cmake_minimum_required(VERSION 3.1.0 FATAL_ERROR)

project(lafrpc)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../qtnetworkng/qtnetworkng.h")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../qtnetworkng/" qtnetworkng EXCLUDE_FROM_ALL)
else()
    add_subdirectory(qtnetworkng qtnetworkng EXCLUDE_FROM_ALL)
endif()

find_package(Qt5Core CONFIG REQUIRED CMAKE_FIND_ROOT_PATH_BOTH)

option(LAFRPC_BUILD_TESTS OFF)
set(CMAKE_AUTOMOC ON)
if (CMAKE_VERSION VERSION_LESS "3.1")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
    else()
        message(SEND_ERROR "Please update your cmake to enable c++11.")
    endif ()
else ()
    set (CMAKE_CXX_STANDARD 11)
endif ()


set(LAFRPC_SRC
    src/peer.cpp
    src/rpc.cpp
    src/serialization.cpp
    src/base.cpp
    src/transport.cpp
    src/sendfile.cpp
    src/senddir.cpp
)

set(LAFRPC_INCLUDE
    include/peer.h
    include/rpc.h
    include/utils.h
    include/serialization.h
    include/serialization_p.h
    include/base.h
    include/transport.h
    include/rpc_p.h
    include/sendfile.h
    include/senddir.h
)

# Fix Qt-static cmake BUG
# https://bugreports.qt.io/browse/QTBUG-38913
if(EXISTS ${_qt5Core_install_prefix}/lib/libQt5Core.a AND NOT EXISTS ${_qt5Core_install_prefix}/bin/Qt5Core.dll)
    message("Static Qt is detected.")
    link_directories(${_qt5Core_install_prefix}/lib/)
endif()


add_library(lafrpc STATIC ${LAFRPC_SRC} ${LAFRPC_INCLUDE})
target_link_libraries(lafrpc PUBLIC qtnetworkng)
target_include_directories(lafrpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(LAFRPC_BUILD_TESTS)
    add_executable(rpctest tests/simple_test.cpp)
    target_link_libraries(rpctest Qt5::Core lafrpc)

    add_executable(sendfiletest tests/sendfile.cpp)
    target_link_libraries(sendfiletest PRIVATE Qt5::Core lafrpc)
endif()
