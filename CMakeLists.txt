# SPDX-License-Identifier: MIT
#
# The C++ half. Only abi_dump exists at T14; the binding, its tests and the
# examples arrive at T39 and after, and get their own add_subdirectory then.

cmake_minimum_required(VERSION 3.20)
project(koru CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# doc/Plan.md's verification sequence names ./build/abi_dump.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Per target, never in CMAKE_CXX_FLAGS: the documented configure line passes
# -DCMAKE_CXX_FLAGS=-fsanitize=..., which replaces that variable wholesale.
set(KORU_WARNINGS -Wall -Wextra -Werror -Wpedantic)

add_executable(abi_dump cpp/tools/abi_dump.cpp)
target_include_directories(abi_dump PRIVATE cpp/include)
target_compile_options(abi_dump PRIVATE ${KORU_WARNINGS})

# koru_abi.h must compile as C11 too, not only as C++.
add_library(koru_abi_c11 OBJECT cpp/tools/abi_c11.c)
target_include_directories(koru_abi_c11 PRIVATE cpp/include)
target_compile_options(koru_abi_c11 PRIVATE ${KORU_WARNINGS})

enable_testing()

# One implementation of the comparison, shared with the command line.
add_test(NAME abi_conformance
         COMMAND ${CMAKE_SOURCE_DIR}/scripts/abi.sh --cpp $<TARGET_FILE:abi_dump>)
set_tests_properties(abi_conformance PROPERTIES LABELS abi)
