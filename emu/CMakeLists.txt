# lex224emu: the lexcore library, the CLI, the lexprobe research tool and the ctest suite.

cmake_minimum_required(VERSION 3.16)

# Out-of-source builds only, so a clean build is just deleting the build directory
# (see "Clean build" in README.md).
get_filename_component(_lex_src "${CMAKE_SOURCE_DIR}" REALPATH)
get_filename_component(_lex_bin "${CMAKE_BINARY_DIR}" REALPATH)
if(_lex_src STREQUAL _lex_bin)
  message(FATAL_ERROR
    "In-source builds are not supported. From the repository root run:\n"
    "  cmake -S emu -B emu/build\n"
    "and delete the CMakeCache.txt and CMakeFiles/ just created in ${CMAKE_SOURCE_DIR}.")
endif()

project(lex224emu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_property(_lex_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _lex_multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

set(LEX224_ROM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../roms/224XL_v8_21")

# Engine core shared by the CLI, the research probe and the tests.
add_library(lexcore STATIC
  src/i8080.cpp
  src/dsp.cpp
  src/machine.cpp
  src/audio.cpp
  src/romfile.cpp
  src/resampler.cpp
  src/engine.cpp
)
target_include_directories(lexcore PUBLIC src)
target_compile_options(lexcore PRIVATE -Wall -Wextra)

add_executable(lex224emu src/main.cpp)
target_link_libraries(lex224emu PRIVATE lexcore)
target_compile_options(lex224emu PRIVATE -Wall -Wextra)
target_compile_definitions(lex224emu PRIVATE LEX224_DEFAULT_ROM_DIR="${LEX224_ROM_DIR}")

# Research probe on the emulated machine (tools/README.md).
add_executable(lexprobe ../tools/lexprobe.cpp)
target_link_libraries(lexprobe PRIVATE lexcore)
target_compile_options(lexprobe PRIVATE -Wall -Wextra)
target_compile_definitions(lexprobe PRIVATE LEX224_DEFAULT_ROM_DIR="${LEX224_ROM_DIR}")

# Tests: ctest --test-dir emu/build (see emu/README.md).
enable_testing()

add_executable(resampler_test tests/resampler_test.cpp)
target_link_libraries(resampler_test PRIVATE lexcore)
target_compile_options(resampler_test PRIVATE -Wall -Wextra)
add_test(NAME resampler COMMAND resampler_test)

add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE lexcore)
target_compile_options(engine_test PRIVATE -Wall -Wextra)
target_compile_definitions(engine_test PRIVATE LEX224_ROM_DIR="${LEX224_ROM_DIR}")
add_test(NAME engine COMMAND engine_test)

add_executable(slider_test tests/slider_test.cpp)
target_link_libraries(slider_test PRIVATE lexcore)
target_compile_options(slider_test PRIVATE -Wall -Wextra)
target_compile_definitions(slider_test PRIVATE LEX224_ROM_DIR="${LEX224_ROM_DIR}")
add_test(NAME slider COMMAND slider_test)

add_executable(aru_test tests/aru_test.cpp)
target_link_libraries(aru_test PRIVATE lexcore)
target_compile_options(aru_test PRIVATE -Wall -Wextra)
target_compile_definitions(aru_test PRIVATE LEX224_ROM_DIR="${LEX224_ROM_DIR}")
add_test(NAME aru COMMAND aru_test)

add_executable(selftest_test tests/selftest_test.cpp)
target_link_libraries(selftest_test PRIVATE lexcore)
target_compile_options(selftest_test PRIVATE -Wall -Wextra)
target_compile_definitions(selftest_test PRIVATE LEX224_ROM_DIR="${LEX224_ROM_DIR}")
add_test(NAME selftest COMMAND selftest_test)

