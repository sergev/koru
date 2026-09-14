# SPDX-License-Identifier: MIT
#
# The C++ half: the two ABI dumps, and from T34 the screen daemon. The koru
# binding itself, its tests and the examples arrive at T39 and after.

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

# The screen protocol, T33. Canonical here, mirrored by
# rust/runtime/src/ks_abi.rs, and diffed by the same script.
add_executable(ks_dump screen/tools/ks_dump.cpp)
target_include_directories(ks_dump PRIVATE screen)
target_compile_options(ks_dump PRIVATE ${KORU_WARNINGS})

add_library(ks_abi_c11 OBJECT screen/tools/ks_abi_c11.c)
target_include_directories(ks_abi_c11 PRIVATE screen)
target_compile_options(ks_abi_c11 PRIVATE ${KORU_WARNINGS})

# T34: the terminal model, headless. No SDL, no socket, no koru — it builds
# and runs on the host with nothing installed.
add_library(ks_model OBJECT screen/screen.cpp screen/ansi.cpp screen/text.cpp)
target_include_directories(ks_model PUBLIC screen)
target_compile_options(ks_model PRIVATE ${KORU_WARNINGS})

# scripts/screen.sh configures a second build directory with this on. It is a
# per-target flag, never CMAKE_CXX_FLAGS, for the reason above.
option(KORU_SANITIZE "build the screen targets with ASan and UBSan" OFF)
option(KORU_FUZZ "build the libFuzzer targets (clang plus its runtime)" OFF)
set(KORU_SAN_FLAGS)
if(KORU_SANITIZE)
  set(KORU_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g)
endif()

# The model's own suite: Braam's cell-exact assertions. The sources are
# compiled again rather than linked, so the sanitizers are this target's.
add_executable(ks_model_test
               screen/screen.cpp screen/ansi.cpp screen/text.cpp
               screen/tests/harness.cpp screen/tests/test_screen.cpp
               screen/tests/test_ansi.cpp)
target_include_directories(ks_model_test PRIVATE screen screen/tests)
target_compile_options(ks_model_test PRIVATE ${KORU_WARNINGS} ${KORU_SAN_FLAGS})
target_link_options(ks_model_test PRIVATE ${KORU_SAN_FLAGS})

# The parser's oracle, driven by a PRNG rather than by coverage. It needs no
# clang runtime, so this is the one that runs in the everyday gate.
add_executable(ks_ansi_rand
               screen/screen.cpp screen/ansi.cpp screen/text.cpp
               screen/tests/fuzz_ansi.cpp)
target_include_directories(ks_ansi_rand PRIVATE screen)
target_compile_definitions(ks_ansi_rand PRIVATE KS_FUZZ_STANDALONE)
target_compile_options(ks_ansi_rand PRIVATE ${KORU_WARNINGS} ${KORU_SAN_FLAGS})
target_link_options(ks_ansi_rand PRIVATE ${KORU_SAN_FLAGS})

# The same oracle under libFuzzer's coverage guidance. libFuzzer is clang's and
# lives in libclang-rt-*-dev, so it is opt-in:
#   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DKORU_FUZZ=ON
if(KORU_FUZZ)
  add_executable(ks_ansi_fuzz
                 screen/screen.cpp screen/ansi.cpp screen/text.cpp
                 screen/tests/fuzz_ansi.cpp)
  target_include_directories(ks_ansi_fuzz PRIVATE screen)
  target_compile_options(ks_ansi_fuzz PRIVATE ${KORU_WARNINGS} -g
                         -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
  target_link_options(ks_ansi_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

enable_testing()

# One implementation of each comparison, shared with the command line.
add_test(NAME abi_conformance
         COMMAND ${CMAKE_SOURCE_DIR}/scripts/abi.sh --cpp $<TARGET_FILE:abi_dump>
                                                    --ks $<TARGET_FILE:ks_dump>)
set_tests_properties(abi_conformance PROPERTIES LABELS abi)

add_test(NAME screen_model COMMAND ks_model_test)
set_tests_properties(screen_model PROPERTIES LABELS screen)

add_test(NAME screen_ansi_rand COMMAND ks_ansi_rand)
set_tests_properties(screen_ansi_rand PROPERTIES LABELS screen)
