# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2023 Meta Platforms, Inc. and affiliates.

enable_language(CXX)

find_package(benchmark REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(bpf REQUIRED IMPORTED_TARGET libbpf)
pkg_check_modules(git2 REQUIRED IMPORTED_TARGET libgit2)

add_executable(benchmark_bin EXCLUDE_FROM_ALL
    main.cpp
    benchmark.cpp benchmark.hpp
)

target_compile_options(benchmark_bin
    PRIVATE
        -std=c++20
)

target_include_directories(benchmark_bin
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(benchmark_bin
    PRIVATE
        PkgConfig::bpf
        PkgConfig::git2
        benchmark::benchmark
)

add_custom_target(benchmark
    COMMAND
        ${CMAKE_COMMAND}
            -E make_directory
            ${CMAKE_BINARY_DIR}/output/benchmarks
    COMMAND
        ${CMAKE_SOURCE_DIR}/tools/asroot
            $<TARGET_FILE:benchmark_bin>
                --cli $<TARGET_FILE:bfcli>
                --daemon $<TARGET_FILE:bpfilter>
                --srcdir ${CMAKE_SOURCE_DIR}
                --outfile ${CMAKE_BINARY_DIR}/output/benchmarks/{gitrev}.json
    DEPENDS benchmark_bin bfcli bpfilter
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running benchmarks"
)
