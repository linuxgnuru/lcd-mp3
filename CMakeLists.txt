cmake_minimum_required(VERSION 2.8)

# project name
project(lcd-mp3)

set(SOURCES ${PROJECT_SOURCE_DIR}/lcd-mp3.c
    ${PROJECT_SOURCE_DIR}/lcd-mp3.h
    ${PROJECT_SOURCE_DIR}/rotaryencoder.c
    ${PROJECT_SOURCE_DIR}/rotaryencoder.h)

set(CMAKE_BINARY_DIR ${CMAKE_SOURCE_DIR}/bin)
#set(CMAKE_SOURCE_DIR ${CMAKE
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})

# The following folder will be included
include_directories("${PROJECT_SOURCE_DIR}")
#add_library(ao mpg123 pthread m wiringPi wiringPiDev asound)

#find_package(ao REQUIRED mpg123 REQUIRED pthread REQUIRED asound REQUIRED)
find_file(/usr/local/include/wiringPi.h /usr/include/wiringPi.h)

add_executable(lcd-mp3 ${PROJECT_SOURCE_DIR}/lcd-mp3.c lcd-mp3.h rotaryencoder.c rotaryencoder.h)
target_link_libraries( lcd-mp3 ao mpg123 pthread m wiringPi wiringPiDev asound)
