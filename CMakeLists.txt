cmake_minimum_required(VERSION 3.6)
project(quick_sort_base_stack)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES
        C_8.cpp
        C_8.h
        C_9.h)

add_executable(quick_sort_base_stack ${SOURCE_FILES})