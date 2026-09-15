# SPDX-License-Identifier: MIT
#
# The C++ half: the two ABI dumps, the screen daemon from T34, and from T39
# libkoru and its device suite. The examples arrive with the surface.

cmake_minimum_required(VERSION 3.20)
project(koru CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# -std=gnu++20, not c++20: Braam's TRY and CO_TRY are statement expressions,
# and C++ has no other way to spell "unwrap or return" as an expression. T44
# is what needed it; doc/Notes.md records the change, and the GCC bug that
# makes `CO_TRY(co_await ...)` clang-only.
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# doc/Plan.md's verification sequence names ./build/abi_dump.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Per target, never in CMAKE_CXX_FLAGS: the documented configure line passes
# -DCMAKE_CXX_FLAGS=-fsanitize=..., which replaces that variable wholesale.
#
# Two sets. **-Wpedantic means "warn about anything not ISO", and since T44 the
# binding's dialect is deliberately not ISO**: Braam's TRY, TRY_VOID, CO_TRY
# and CO_TRY_VOID are statement expressions, which is the only way C++ has to
# spell "unwrap or return" as an expression. Both compilers refuse one under
# -Wpedantic — GCC as an error at the `({`, clang at every use site — and
# neither can be silenced from inside the header, because the diagnostic lands
# in the user's translation unit and not in the macro's.
#
# So anything that includes koru/vocab.hpp gets KORU_WARNINGS, and everything
# that does not — the two ABI mirrors, the C11 probes and the whole screen
# daemon — keeps KORU_PEDANTIC, where the ISO check is worth having.
set(KORU_WARNINGS -Wall -Wextra -Werror)
set(KORU_PEDANTIC ${KORU_WARNINGS} -Wpedantic)

add_executable(abi_dump cpp/tools/abi_dump.cpp)
target_include_directories(abi_dump PRIVATE cpp/include)
target_compile_options(abi_dump PRIVATE ${KORU_PEDANTIC})

# koru_abi.h must compile as C11 too, not only as C++.
add_library(koru_abi_c11 OBJECT cpp/tools/abi_c11.c)
target_include_directories(koru_abi_c11 PRIVATE cpp/include)
target_compile_options(koru_abi_c11 PRIVATE ${KORU_PEDANTIC})

# The screen protocol, T33. Canonical here, mirrored by
# rust/runtime/src/ks_abi.rs, and diffed by the same script.
add_executable(ks_dump screen/tools/ks_dump.cpp)
target_include_directories(ks_dump PRIVATE screen)
target_compile_options(ks_dump PRIVATE ${KORU_PEDANTIC})

add_library(ks_abi_c11 OBJECT screen/tools/ks_abi_c11.c)
target_include_directories(ks_abi_c11 PRIVATE screen)
target_compile_options(ks_abi_c11 PRIVATE ${KORU_PEDANTIC})

# T34: the terminal model, headless. No SDL, no socket, no koru — it builds
# and runs on the host with nothing installed.
add_library(ks_model OBJECT screen/screen.cpp screen/ansi.cpp screen/text.cpp)
target_include_directories(ks_model PUBLIC screen)
target_compile_options(ks_model PRIVATE ${KORU_PEDANTIC})

# scripts/screen.sh configures a second build directory with this on. It is a
# per-target flag, never CMAKE_CXX_FLAGS, for the reason above.
option(KORU_SANITIZE "build the screen targets with ASan and UBSan" OFF)
option(KORU_FUZZ "build the libFuzzer targets (clang plus its runtime)" OFF)
set(KORU_SAN_FLAGS)
if(KORU_SANITIZE)
  # -O2 is not decoration. The sanitizers are documented as wanting
  # optimization on, and GCC performs a coroutine's symmetric transfer only
  # with it: at -O0 and -O1 a hundred thousand nested awaits cost a stack
  # frame each. doc/Notes.md has the measurements.
  set(KORU_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -g -O2)
endif()

# T39: libkoru, the synchronous core. RAII `Ring` and `Arena`, the buffer pool,
# `result<T>` and the errno table, on the same unchanged ABI the Rust binding
# uses. It builds anywhere; only its suite needs the device.
#
# T44-T48 add the surface itself on top of it: the vocabulary and the ambient
# ring, the operation layer, the buffered stream, the program shell and the
# screen client. All of it is Braam's, and `cpp/include/koru/braam.hpp` hoists
# it to global scope.
add_library(koru STATIC cpp/src/error.cpp cpp/src/ring.cpp cpp/src/pool.cpp
                        cpp/src/reactor.cpp cpp/src/exec.cpp cpp/src/args.cpp
                        cpp/src/rt.cpp cpp/src/ops.cpp cpp/src/filebuf.cpp
                        cpp/src/file.cpp cpp/src/iter.cpp cpp/src/opt.cpp
                        cpp/src/usage.cpp cpp/src/time.cpp cpp/src/grid.cpp
                        cpp/src/textbuf.cpp cpp/src/screen.cpp cpp/src/text.cpp
                        cpp/src/path.cpp cpp/src/size.cpp cpp/src/ftoa.cpp)
target_include_directories(koru PUBLIC cpp/include)
# ks_abi.h is the screen protocol's canonical mirror, and the client's cell is
# its `ks_cell`: that is what makes the read-a-blit-in-place claim true.
target_include_directories(koru PUBLIC screen)
# -O2 for the same reason koru_cpp_unit has it: GCC performs a coroutine's
# symmetric transfer only with optimization on, and every operation above is a
# coroutine awaiting another.
target_compile_options(koru PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS})

# The `main` a koru program does not write. Its own library, so a binary with
# a `main` of its own can link libkoru without getting a second one.
add_library(koru_start STATIC cpp/src/main.cpp)
target_link_libraries(koru_start PUBLIC koru)
target_compile_options(koru_start PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS})

# The same for a Braam `src/cmd` source, whose entry is `proc_main` and whose
# status is a bare `i32`. T49's programs link this one.
add_library(koru_start_proc STATIC cpp/src/main.cpp cpp/src/proc_main.cpp)
target_link_libraries(koru_start_proc PUBLIC koru)
target_compile_options(koru_start_proc PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS})

# The device-free half: the slab, the op state machine and the vocabulary. It
# needs no module, so ctest runs it on the host.
add_executable(koru_cpp_unit cpp/tests/harness.cpp cpp/tests/unit.cpp
                             cpp/tests/task.cpp cpp/tests/vocab.cpp
                             cpp/tests/shell.cpp cpp/tests/ui.cpp)
target_include_directories(koru_cpp_unit PRIVATE cpp/tests)
target_link_libraries(koru_cpp_unit PRIVATE koru)
# -O2 even in the plain build: GCC performs a coroutine's symmetric transfer
# only with optimization on, and the task cases measure exactly that. Without
# it this target is testing the compiler's debug codegen instead.
target_compile_options(koru_cpp_unit PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS})
target_link_options(koru_cpp_unit PRIVATE ${KORU_SAN_FLAGS})

# The device suite, mirroring rust/sys/tests/kernel.rs case for case. It needs
# the module, so scripts/run-cpp.sh runs it in the VM.
add_executable(koru_cpp_check cpp/tests/harness.cpp cpp/tests/common.cpp
                              cpp/tests/kernel.cpp cpp/tests/reactor.cpp
                              cpp/tests/drop.cpp cpp/tests/ops.cpp
                              cpp/tests/file.cpp cpp/tests/screen.cpp)
target_include_directories(koru_cpp_check PRIVATE cpp/tests)
target_link_libraries(koru_cpp_check PRIVATE koru)
target_compile_options(koru_cpp_check PRIVATE ${KORU_WARNINGS} ${KORU_SAN_FLAGS})
target_link_options(koru_cpp_check PRIVATE ${KORU_SAN_FLAGS})

# T42's demo: T15's, in C++, and the only thing in this build that is a
# program rather than a test. scripts/cpp.sh runs it beside the Rust one and
# compares the bytes.
add_executable(cpp_read_file cpp/examples/read_file.cpp)
target_link_libraries(cpp_read_file PRIVATE koru)
target_compile_options(cpp_read_file PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS})
target_link_options(cpp_read_file PRIVATE ${KORU_SAN_FLAGS})

# T44's: Braam's hello world, in Braam style, with `braam.hpp` the only
# include and `koru_start` the `main` it does not write.
add_executable(cpp_hello cpp/examples/hello.cpp)
target_link_libraries(cpp_hello PRIVATE koru_start)
target_compile_options(cpp_hello PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS})
target_link_options(cpp_hello PRIVATE ${KORU_SAN_FLAGS})

# T47's: Braam's `date`, the program shell end to end. Which stream a usage
# block goes to and what status follows it is a program's question, so it
# cannot be asked of a library; scripts/cpp.sh runs this with redirections.
add_executable(cpp_date cpp/examples/date.cpp)
target_link_libraries(cpp_date PRIVATE koru_start)
target_compile_options(cpp_date PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS})
target_link_options(cpp_date PRIVATE ${KORU_SAN_FLAGS})

# T48's: Braam's pager, painting a real window through the daemon.
# scripts/e2e.sh runs it beside the Rust one and compares the two window
# snapshots byte for byte.
add_executable(cpp_less cpp/examples/less.cpp)
target_link_libraries(cpp_less PRIVATE koru_start)
target_compile_options(cpp_less PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS})
target_link_options(cpp_less PRIVATE ${KORU_SAN_FLAGS})

# T49: Braam's own `src/cmd` programs, each with its include block replaced by
# `<koru/braam.hpp>` and nothing else changed. They are the portability proof,
# so they are listed one by one rather than globbed — a source that stopped
# compiling must break the build, not quietly leave the list.
set(KORU_CMD echo basename dirname pwd seq sleep touch mkdir rm ln truncate
             date cat wc head tail tr cut uniq cmp grep)
# Braam writes `move(x)` unqualified, because its own `move` is at global
# scope; here the name is `std::move`, and clang warns on every such call under
# -Wunqualified-std-cast-call. The warning is about this binding's hoisting and
# not about the program, and it is the only flag any of these sources needs.
foreach(prog ${KORU_CMD})
  add_executable(cmd_${prog} cpp/cmd/${prog}.cpp)
  target_link_libraries(cmd_${prog} PRIVATE koru_start_proc)
  target_compile_options(cmd_${prog} PRIVATE ${KORU_WARNINGS} -O2 ${KORU_SAN_FLAGS}
    $<$<CXX_COMPILER_ID:Clang>:-Wno-unqualified-std-cast-call>)
  target_link_options(cmd_${prog} PRIVATE ${KORU_SAN_FLAGS})
endforeach()
add_custom_target(koru_cmd DEPENDS)
foreach(prog ${KORU_CMD})
  add_dependencies(koru_cmd cmd_${prog})
endforeach()

# SDL3 is the renderer's, and only the renderer's: the model and the protocol
# build without it, which is what lets most of this gate run anywhere.
find_package(SDL3 QUIET CONFIG)
if(NOT SDL3_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL3 IMPORTED_TARGET sdl3)
    if(SDL3_FOUND)
      add_library(SDL3::SDL3 ALIAS PkgConfig::SDL3)
    endif()
  endif()
endif()

# The model's own suite: Braam's cell-exact assertions, plus the pixel oracles
# where SDL3 is installed. The sources are compiled again rather than linked,
# so the sanitizers are this target's.
set(KS_TEST_SRC screen/screen.cpp screen/ansi.cpp screen/text.cpp screen/proto.cpp
                screen/tests/harness.cpp screen/tests/test_screen.cpp
                screen/tests/test_ansi.cpp screen/tests/test_proto.cpp)
if(SDL3_FOUND)
  list(APPEND KS_TEST_SRC screen/render.cpp screen/window.cpp
                          screen/tests/test_render.cpp)
endif()

add_executable(ks_model_test ${KS_TEST_SRC})
target_include_directories(ks_model_test PRIVATE screen screen/tests)
target_compile_options(ks_model_test PRIVATE ${KORU_PEDANTIC} ${KORU_SAN_FLAGS})
target_link_options(ks_model_test PRIVATE ${KORU_SAN_FLAGS})
if(SDL3_FOUND)
  target_compile_definitions(ks_model_test PRIVATE KS_HAVE_SDL)
  target_link_libraries(ks_model_test PRIVATE SDL3::SDL3)
endif()

# The daemon itself: a window, a terminal and the socket clients reach it over.
if(SDL3_FOUND)
  add_executable(koru-screen
                 screen/screen.cpp screen/ansi.cpp screen/text.cpp screen/proto.cpp
                 screen/render.cpp screen/window.cpp screen/ks_daemon.cpp)
  target_include_directories(koru-screen PRIVATE screen)
  target_compile_options(koru-screen PRIVATE ${KORU_PEDANTIC} ${KORU_SAN_FLAGS})
  target_link_options(koru-screen PRIVATE ${KORU_SAN_FLAGS})
  target_link_libraries(koru-screen PRIVATE SDL3::SDL3)
endif()

# Reads one of SDL's snapshots, so a shell script can assert about pixels.
# No SDL: it parses the BMP itself, which is what lets it run where the window
# is long gone.
add_executable(ks_pixel screen/tools/ks_pixel.cpp)
target_compile_options(ks_pixel PRIVATE ${KORU_PEDANTIC} ${KORU_SAN_FLAGS})
target_link_options(ks_pixel PRIVATE ${KORU_SAN_FLAGS})

# A window over a terminal fed from stdin: the event pump and the resize path,
# and how a human looks at the renderer.
if(SDL3_FOUND)
  add_executable(ks_show screen/screen.cpp screen/ansi.cpp screen/text.cpp
                         screen/render.cpp screen/window.cpp screen/tools/ks_show.cpp)
  target_include_directories(ks_show PRIVATE screen)
  target_compile_options(ks_show PRIVATE ${KORU_PEDANTIC} ${KORU_SAN_FLAGS})
  target_link_options(ks_show PRIVATE ${KORU_SAN_FLAGS})
  target_link_libraries(ks_show PRIVATE SDL3::SDL3)
endif()

# The parser's oracle, driven by a PRNG rather than by coverage. It needs no
# clang runtime, so this is the one that runs in the everyday gate.
add_executable(ks_ansi_rand
               screen/screen.cpp screen/ansi.cpp screen/text.cpp
               screen/tests/fuzz_ansi.cpp)
target_include_directories(ks_ansi_rand PRIVATE screen)
target_compile_definitions(ks_ansi_rand PRIVATE KS_FUZZ_STANDALONE)
target_compile_options(ks_ansi_rand PRIVATE ${KORU_PEDANTIC} ${KORU_SAN_FLAGS})
target_link_options(ks_ansi_rand PRIVATE ${KORU_SAN_FLAGS})

# The protocol server's oracle, the same shape.
add_executable(ks_proto_rand
               screen/screen.cpp screen/ansi.cpp screen/text.cpp screen/proto.cpp
               screen/tests/fuzz_proto.cpp)
target_include_directories(ks_proto_rand PRIVATE screen)
target_compile_definitions(ks_proto_rand PRIVATE KS_FUZZ_STANDALONE)
target_compile_options(ks_proto_rand PRIVATE ${KORU_PEDANTIC} ${KORU_SAN_FLAGS})
target_link_options(ks_proto_rand PRIVATE ${KORU_SAN_FLAGS})

# The same oracle under libFuzzer's coverage guidance. libFuzzer is clang's and
# lives in libclang-rt-*-dev, so it is opt-in:
#   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DKORU_FUZZ=ON
if(KORU_FUZZ)
  foreach(target ansi proto)
    add_executable(ks_${target}_fuzz
                   screen/screen.cpp screen/ansi.cpp screen/text.cpp screen/proto.cpp
                   screen/tests/fuzz_${target}.cpp)
    target_include_directories(ks_${target}_fuzz PRIVATE screen)
    target_compile_options(ks_${target}_fuzz PRIVATE ${KORU_PEDANTIC} -g
                           -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(ks_${target}_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()

enable_testing()

# One implementation of each comparison, shared with the command line.
add_test(NAME abi_conformance
         COMMAND ${CMAKE_SOURCE_DIR}/scripts/abi.sh --cpp $<TARGET_FILE:abi_dump>
                                                    --ks $<TARGET_FILE:ks_dump>)
set_tests_properties(abi_conformance PROPERTIES LABELS abi)

add_test(NAME cpp_unit COMMAND koru_cpp_unit)
set_tests_properties(cpp_unit PROPERTIES LABELS cpp)

add_test(NAME screen_model COMMAND ks_model_test)
set_tests_properties(screen_model PROPERTIES LABELS screen)

add_test(NAME screen_ansi_rand COMMAND ks_ansi_rand)
set_tests_properties(screen_ansi_rand PROPERTIES LABELS screen)

add_test(NAME screen_proto_rand COMMAND ks_proto_rand)
set_tests_properties(screen_proto_rand PROPERTIES LABELS screen)
