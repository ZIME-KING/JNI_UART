cmake_minimum_required(VERSION 3.10)

project(carserial LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8)
endif()

set(CAR_SERIAL_CORE_SOURCES
    src/CarSerialService.cpp
    src/FrameCodec.cpp
    src/FrameStreamParser.cpp
    src/LinkManager.cpp
    src/Log.cpp
    src/SerialPort.cpp
)

if (ANDROID)
    add_library(carserial SHARED
        src/jni_bridge.cpp
        ${CAR_SERIAL_CORE_SOURCES}
    )

    target_include_directories(carserial
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    find_library(log-lib log)
    if(log-lib)
        target_link_libraries(carserial ${log-lib})
    endif()

    add_executable(carserial_host_tool
        src/host_main.cpp
        ${CAR_SERIAL_CORE_SOURCES}
    )

    target_include_directories(carserial_host_tool
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(log-lib)
        target_link_libraries(carserial_host_tool ${log-lib})
    endif()
endif()

if (NOT ANDROID)
    add_executable(carserial_host_tool
        src/host_main.cpp
        ${CAR_SERIAL_CORE_SOURCES}
    )

    target_include_directories(carserial_host_tool
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()

