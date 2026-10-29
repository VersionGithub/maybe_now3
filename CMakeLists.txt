cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(FrameWindows VERSION 0.1.0)

file(GLOB_RECURSE SOURCES src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# библиотека формата .gdr2 (должна идти ПОСЛЕ add_subdirectory и ДО setup_geode_mod)
CPMAddPackage("gh:maxnut/GDReplayFormat#d8d2f0b1193a46cdf47b413dc832616dd052b6a9")
target_link_libraries(${PROJECT_NAME} libGDR)

setup_geode_mod(${PROJECT_NAME})
