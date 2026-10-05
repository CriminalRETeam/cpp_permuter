cmake_minimum_required(VERSION 3.16)
# static MSVC runtime (CMAKE_MSVC_RUNTIME_LIBRARY): the .exe runs without the
# Visual C++ redistributable
cmake_policy(SET CMP0091 NEW)
project(cpp_permuter CXX)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(permuter_core STATIC
    src/lexer.cpp
    src/parser.cpp
    src/analysis.cpp
    src/passes.cpp
    src/passes_control.cpp
    src/passes_more.cpp
    src/passes_manual.cpp
    src/passes_extra.cpp
    src/scorer.cpp
    src/runner.cpp
    src/workspace.cpp
    src/macros.cpp
    src/examples.cpp
)
target_include_directories(permuter_core PUBLIC src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(permuter_core PRIVATE -Wall -Wextra)
endif()

add_executable(cpp_permuter src/main.cpp)
target_link_libraries(cpp_permuter PRIVATE permuter_core Threads::Threads)

enable_testing()
add_executable(permuter_tests tests/tests.cpp)
target_link_libraries(permuter_tests PRIVATE permuter_core Threads::Threads)
target_compile_definitions(permuter_tests PRIVATE PERMUTER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unit COMMAND permuter_tests)
# integration: clang stand-in, runs anywhere clang and llvm-objdump are
# installed. gta2_parse and vc6 need GTA2_RE (and wine for vc6). Exit code 77
# means "skipped": the tools or the checkout aren't available here.
foreach(suite integration gta2_parse vc6)
    if(suite STREQUAL "integration")
        set(script run.sh)
    else()
        set(script ${suite}.sh)
    endif()
    add_test(NAME ${suite}
             COMMAND ${CMAKE_COMMAND} -E env PERMUTER=$<TARGET_FILE:cpp_permuter>
                     sh ${CMAKE_SOURCE_DIR}/tests/integration/${script})
    set_tests_properties(${suite} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
