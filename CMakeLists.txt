cmake_minimum_required(VERSION 3.16)
project(cpp_permuter CXX)

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
    src/scorer.cpp
    src/runner.cpp
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
add_test(NAME unit COMMAND permuter_tests)
add_test(NAME end_to_end
         COMMAND ${CMAKE_COMMAND} -E env PERMUTER=$<TARGET_FILE:cpp_permuter>
                 sh ${CMAKE_SOURCE_DIR}/tests/e2e/run.sh)
