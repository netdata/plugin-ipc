cmake_minimum_required(VERSION 3.20)

project(plugin_ipc LANGUAGES C)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

include(CTest)

set(NETIPC_WINDOWS_RUNTIME OFF)
if(WIN32 OR CYGWIN OR CMAKE_SYSTEM_NAME STREQUAL "MSYS")
    set(NETIPC_WINDOWS_RUNTIME ON)
endif()

if(NETIPC_WINDOWS_RUNTIME)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
            RESULT_VARIABLE NETIPC_C_COMPILER_TARGET_RC
            OUTPUT_VARIABLE NETIPC_C_COMPILER_TARGET
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NETIPC_C_COMPILER_TARGET_RC EQUAL 0)
            if(NETIPC_C_COMPILER_TARGET MATCHES "(msys|cygwin)")
                message(STATUS
                    "Using the Windows IPC backend with a POSIX-emulated Windows C runtime target "
                    "`${NETIPC_C_COMPILER_TARGET}`."
                )
            else()
                message(STATUS
                    "Using the Windows IPC backend with native Windows target "
                    "`${NETIPC_C_COMPILER_TARGET}`."
                )
            endif()
        endif()
    endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(NETIPC_BUILD_FIXTURES "Build C fixture binaries" ON)
option(NETIPC_BUILD_HELPERS "Build Go/Rust helper binaries via CMake" ON)
option(NETIPC_REGISTER_SCRIPT_WORKFLOWS "Register shell-script validation and benchmark workflows with CMake" ON)

find_package(Threads REQUIRED)

add_subdirectory(src/libnetdata/netipc)

if(NETIPC_BUILD_HELPERS)
    find_program(CARGO_EXECUTABLE cargo)
    find_program(GO_EXECUTABLE go)

    file(GLOB_RECURSE NETIPC_RUST_CRATE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/crates/netipc/src/*.rs"
        "${CMAKE_SOURCE_DIR}/src/crates/netipc/Cargo.toml"
    )
    file(GLOB_RECURSE NETIPC_GO_PACKAGE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/go/pkg/netipc/*.go"
        "${CMAKE_SOURCE_DIR}/src/go/go.mod"
    )

    set(NETIPC_RUST_FIXTURES_MANIFEST "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/Cargo.toml")
    set(NETIPC_RUST_FIXTURES_TARGET "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/target/release")
    set(NETIPC_CODEC_RS_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc-codec-rs${CMAKE_EXECUTABLE_SUFFIX}")
    set(NETIPC_LIVE_RS_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc_live_rs${CMAKE_EXECUTABLE_SUFFIX}")

    if(CARGO_EXECUTABLE)
        add_custom_command(
            OUTPUT "${NETIPC_CODEC_RS_BIN}"
            COMMAND "${CARGO_EXECUTABLE}" build --release --bin netipc-codec-rs --manifest-path "${NETIPC_RUST_FIXTURES_MANIFEST}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${NETIPC_RUST_FIXTURES_TARGET}/netipc-codec-rs${CMAKE_EXECUTABLE_SUFFIX}"
                    "${NETIPC_CODEC_RS_BIN}"
            DEPENDS
                "${NETIPC_RUST_FIXTURES_MANIFEST}"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/build.rs"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/src/bin/netipc_codec_rs.rs"
                ${NETIPC_RUST_CRATE_SOURCES}
            VERBATIM
        )

        add_custom_target(netipc-codec-rs DEPENDS "${NETIPC_CODEC_RS_BIN}")

        if(NOT NETIPC_WINDOWS_RUNTIME)
            add_custom_command(
                OUTPUT "${NETIPC_LIVE_RS_BIN}"
                COMMAND "${CARGO_EXECUTABLE}" build --release --bin netipc_live_rs --manifest-path "${NETIPC_RUST_FIXTURES_MANIFEST}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${NETIPC_RUST_FIXTURES_TARGET}/netipc_live_rs${CMAKE_EXECUTABLE_SUFFIX}"
                        "${NETIPC_LIVE_RS_BIN}"
                DEPENDS
                    "${NETIPC_RUST_FIXTURES_MANIFEST}"
                    "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/build.rs"
                    "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/src/bin/netipc_live_rs.rs"
                    ${NETIPC_RUST_CRATE_SOURCES}
                VERBATIM
            )

            add_custom_target(netipc_live_rs DEPENDS "${NETIPC_LIVE_RS_BIN}")
        endif()

        set(NETIPC_RUST_BENCH_MANIFEST "${CMAKE_SOURCE_DIR}/bench/drivers/rust/Cargo.toml")
        set(NETIPC_RUST_BENCH_TARGET "${CMAKE_SOURCE_DIR}/bench/drivers/rust/target/release")
        set(NETIPC_LIVE_UDS_RS_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc_live_uds_rs")

        add_custom_command(
            OUTPUT "${NETIPC_LIVE_UDS_RS_BIN}"
            COMMAND "${CARGO_EXECUTABLE}" build --release --bin netipc_live_uds_rs --manifest-path "${NETIPC_RUST_BENCH_MANIFEST}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${NETIPC_RUST_BENCH_TARGET}/netipc_live_uds_rs"
                    "${NETIPC_LIVE_UDS_RS_BIN}"
            DEPENDS
                "${NETIPC_RUST_BENCH_MANIFEST}"
                "${CMAKE_SOURCE_DIR}/bench/drivers/rust/netipc_live_uds_rs.rs"
                ${NETIPC_RUST_CRATE_SOURCES}
            VERBATIM
        )

        add_custom_target(netipc_live_uds_rs DEPENDS "${NETIPC_LIVE_UDS_RS_BIN}")

        if(NETIPC_WINDOWS_RUNTIME)
            set(NETIPC_LIVE_WIN_RS_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc_live_win_rs${CMAKE_EXECUTABLE_SUFFIX}")
            add_custom_command(
                OUTPUT "${NETIPC_LIVE_WIN_RS_BIN}"
                COMMAND "${CARGO_EXECUTABLE}" build --release --features windows-driver --bin netipc_live_win_rs --manifest-path "${NETIPC_RUST_BENCH_MANIFEST}"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${NETIPC_RUST_BENCH_TARGET}/netipc_live_win_rs${CMAKE_EXECUTABLE_SUFFIX}"
                        "${NETIPC_LIVE_WIN_RS_BIN}"
                DEPENDS
                    "${NETIPC_RUST_BENCH_MANIFEST}"
                    "${CMAKE_SOURCE_DIR}/bench/drivers/rust/netipc_live_win_rs.rs"
                    ${NETIPC_RUST_CRATE_SOURCES}
                VERBATIM
            )
            add_custom_target(netipc_live_win_rs DEPENDS "${NETIPC_LIVE_WIN_RS_BIN}")
        endif()
    endif()

    if(GO_EXECUTABLE)
        set(NETIPC_GO_FIXTURE_SOURCES
            "${CMAKE_SOURCE_DIR}/tests/fixtures/go/main.go"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/go/bench_common.go"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/go/bench_time_unix.go"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/go/bench_time_windows.go"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/go/cgroups_bench.go"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/go/cgroups_server_common.go"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/go/cgroups_server_unix.go"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/go/cgroups_server_windows.go"
        )

        set(NETIPC_CODEC_GO_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc-codec-go${CMAKE_EXECUTABLE_SUFFIX}")
        add_custom_command(
            OUTPUT "${NETIPC_CODEC_GO_BIN}"
            COMMAND "${CMAKE_COMMAND}" -E chdir "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
                    "${GO_EXECUTABLE}" build -o "${NETIPC_CODEC_GO_BIN}" .
            DEPENDS
                "${CMAKE_SOURCE_DIR}/tests/fixtures/go/go.mod"
                ${NETIPC_GO_FIXTURE_SOURCES}
                ${NETIPC_GO_PACKAGE_SOURCES}
            VERBATIM
        )
        add_custom_target(netipc-codec-go DEPENDS "${NETIPC_CODEC_GO_BIN}")

        set(NETIPC_LIVE_GO_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc-live-go")
        add_custom_command(
            OUTPUT "${NETIPC_LIVE_GO_BIN}"
            COMMAND "${CMAKE_COMMAND}" -E chdir "${CMAKE_SOURCE_DIR}/bench/drivers/go"
                    "${GO_EXECUTABLE}" build -o "${NETIPC_LIVE_GO_BIN}" .
            DEPENDS
                "${CMAKE_SOURCE_DIR}/bench/drivers/go/go.mod"
                "${CMAKE_SOURCE_DIR}/bench/drivers/go/main.go"
                ${NETIPC_GO_PACKAGE_SOURCES}
            VERBATIM
        )
        add_custom_target(netipc-live-go DEPENDS "${NETIPC_LIVE_GO_BIN}")

        if(NETIPC_WINDOWS_RUNTIME)
            set(NETIPC_LIVE_GO_WIN_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc-live-go-win${CMAKE_EXECUTABLE_SUFFIX}")
            add_custom_command(
                OUTPUT "${NETIPC_LIVE_GO_WIN_BIN}"
                COMMAND "${CMAKE_COMMAND}" -E chdir "${CMAKE_SOURCE_DIR}/bench/drivers/go-win"
                        "${GO_EXECUTABLE}" build -o "${NETIPC_LIVE_GO_WIN_BIN}" .
                DEPENDS
                    "${CMAKE_SOURCE_DIR}/bench/drivers/go-win/go.mod"
                    "${CMAKE_SOURCE_DIR}/bench/drivers/go-win/main.go"
                    ${NETIPC_GO_PACKAGE_SOURCES}
                VERBATIM
            )
            add_custom_target(netipc-live-go-win DEPENDS "${NETIPC_LIVE_GO_WIN_BIN}")
        endif()
    endif()
endif()

add_custom_target(netipc-clean-generated
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "${CMAKE_SOURCE_DIR}/src/crates/target"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/target"
        "${CMAKE_SOURCE_DIR}/bench/drivers/rust/target"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/go/netipc-codec-go"
        "${CMAKE_SOURCE_DIR}/bench/drivers/go/netipc-live-go"
    USES_TERMINAL
    VERBATIM
)

if(NETIPC_REGISTER_SCRIPT_WORKFLOWS)
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        function(netipc_add_script_workflow target_name script_path)
            set(options BENCHMARK)
            set(oneValueArgs TEST_NAME)
            set(multiValueArgs DEPENDS LABELS)
            cmake_parse_arguments(NETIPC_WORKFLOW "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

            foreach(dep IN LISTS NETIPC_WORKFLOW_DEPENDS)
                if(NOT TARGET "${dep}")
                    message(STATUS "Skipping ${target_name}: missing dependency target `${dep}`")
                    return()
                endif()
            endforeach()

            add_custom_target("${target_name}"
                COMMAND "${CMAKE_COMMAND}" -E env
                    "NETIPC_CMAKE_BUILD_DIR=${CMAKE_BINARY_DIR}"
                    "NETIPC_SKIP_CONFIGURE=1"
                    "NETIPC_SKIP_BUILD=1"
                    "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/${script_path}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                DEPENDS ${NETIPC_WORKFLOW_DEPENDS}
                USES_TERMINAL
                VERBATIM
            )

            if(BUILD_TESTING AND NOT NETIPC_WORKFLOW_BENCHMARK)
                if(NOT NETIPC_WORKFLOW_TEST_NAME)
                    set(NETIPC_WORKFLOW_TEST_NAME "${target_name}")
                endif()

                add_test(
                    NAME "${NETIPC_WORKFLOW_TEST_NAME}"
                    COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target "${target_name}"
                )

                if(NETIPC_WORKFLOW_LABELS)
                    set_tests_properties("${NETIPC_WORKFLOW_TEST_NAME}" PROPERTIES
                        LABELS "${NETIPC_WORKFLOW_LABELS}"
                    )
                endif()
            endif()
        endfunction()

        add_custom_target(netipc-check)
        add_custom_target(netipc-bench)
        add_custom_target(netipc-validate-all)
        add_dependencies(netipc-validate-all netipc-check netipc-bench)

        netipc_add_script_workflow(
            netipc-test-interop
            tests/run-interop.sh
            TEST_NAME netipc-interop
            LABELS "interop;schema"
            DEPENDS netipc-codec-c netipc-codec-rs netipc-codec-go
        )
        if(TARGET netipc-test-interop)
            add_dependencies(netipc-check netipc-test-interop)
        endif()

        if(NOT NETIPC_WINDOWS_RUNTIME)
            netipc_add_script_workflow(
                netipc-test-live-interop
                tests/run-live-interop.sh
                TEST_NAME netipc-live-interop
                LABELS "interop;shm"
                DEPENDS netipc-live-c netipc_live_rs
            )
            if(TARGET netipc-test-live-interop)
                add_dependencies(netipc-check netipc-test-live-interop)
            endif()
        endif()

        netipc_add_script_workflow(
            netipc-test-live-uds-interop
            tests/run-live-uds-interop.sh
            TEST_NAME netipc-live-uds-interop
            LABELS "interop;uds"
            DEPENDS netipc-live-c netipc_live_uds_rs netipc-live-go
        )
        if(TARGET netipc-test-live-uds-interop)
            add_dependencies(netipc-check netipc-test-live-uds-interop)
        endif()

        netipc_add_script_workflow(
            netipc-test-uds-seqpacket
            tests/run-uds-seqpacket.sh
            TEST_NAME netipc-uds-seqpacket
            LABELS "protocol;uds"
            DEPENDS netipc-live-c
        )
        if(TARGET netipc-test-uds-seqpacket)
            add_dependencies(netipc-check netipc-test-uds-seqpacket)
        endif()

        netipc_add_script_workflow(
            netipc-test-uds-negotiation-negative
            tests/run-uds-negotiation-negative.sh
            TEST_NAME netipc-uds-negotiation-negative
            LABELS "protocol;negative;uds"
            DEPENDS netipc-live-c netipc_live_uds_rs netipc-live-go
        )
        if(TARGET netipc-test-uds-negotiation-negative)
            add_dependencies(netipc-check netipc-test-uds-negotiation-negative)
        endif()

        if(NOT NETIPC_WINDOWS_RUNTIME)
            netipc_add_script_workflow(
                netipc-test-live-cgroups-baseline
                tests/run-live-cgroups-baseline.sh
                TEST_NAME netipc-live-cgroups-baseline
                LABELS "protocol;service;cache;uds"
                DEPENDS netipc-cgroups-live-c netipc-codec-go netipc-codec-rs
            )
            if(TARGET netipc-test-live-cgroups-baseline)
                add_dependencies(netipc-check netipc-test-live-cgroups-baseline)
            endif()

            netipc_add_script_workflow(
                netipc-test-live-cgroups-shm
                tests/run-live-cgroups-shm.sh
                TEST_NAME netipc-live-cgroups-shm
                LABELS "protocol;service;cache;shm"
                DEPENDS netipc-cgroups-live-c netipc-codec-go netipc-codec-rs
            )
            if(TARGET netipc-test-live-cgroups-shm)
                add_dependencies(netipc-check netipc-test-live-cgroups-shm)
            endif()
        endif()

        if(NETIPC_WINDOWS_RUNTIME)
            netipc_add_script_workflow(
                netipc-test-live-npipe-smoke
                tests/run-live-npipe-smoke.sh
                TEST_NAME netipc-live-npipe-smoke
                LABELS "smoke;windows"
                DEPENDS netipc-live-c
            )
            if(TARGET netipc-test-live-npipe-smoke)
                add_dependencies(netipc-check netipc-test-live-npipe-smoke)
            endif()

            netipc_add_script_workflow(
                netipc-test-live-cgroups-win
                tests/run-live-cgroups-win.sh
                TEST_NAME netipc-live-cgroups-win
                LABELS "service;cache;windows"
                DEPENDS netipc-cgroups-live-c netipc-codec-go netipc-codec-rs
            )
            if(TARGET netipc-test-live-cgroups-win)
                add_dependencies(netipc-check netipc-test-live-cgroups-win)
            endif()
        endif()

        if(NOT NETIPC_WINDOWS_RUNTIME)
            netipc_add_script_workflow(
                netipc-bench-live-shm
                tests/run-live-shm-bench.sh
                BENCHMARK
                DEPENDS netipc-live-c netipc_live_rs
            )
            if(TARGET netipc-bench-live-shm)
                add_dependencies(netipc-bench netipc-bench-live-shm)
            endif()
        endif()

        netipc_add_script_workflow(
            netipc-bench-live-uds
            tests/run-live-uds-bench.sh
            BENCHMARK
            DEPENDS netipc-live-c netipc_live_uds_rs netipc-live-go
        )
        if(TARGET netipc-bench-live-uds)
            add_dependencies(netipc-bench netipc-bench-live-uds)
        endif()

        if(NOT NETIPC_WINDOWS_RUNTIME)
            netipc_add_script_workflow(
                netipc-bench-live-cgroups
                tests/run-live-cgroups-bench.sh
                BENCHMARK
                DEPENDS netipc-cgroups-live-c netipc-codec-go netipc-codec-rs
            )
            if(TARGET netipc-bench-live-cgroups)
                add_dependencies(netipc-bench netipc-bench-live-cgroups)
            endif()

            netipc_add_script_workflow(
                netipc-bench-live-cgroups-shm
                tests/run-live-cgroups-shm-bench.sh
                BENCHMARK
                DEPENDS netipc-cgroups-live-c netipc-codec-go netipc-codec-rs
            )
            if(TARGET netipc-bench-live-cgroups-shm)
                add_dependencies(netipc-bench netipc-bench-live-cgroups-shm)
            endif()
        endif()

        if(NETIPC_WINDOWS_RUNTIME)
            netipc_add_script_workflow(
                netipc-bench-live-cgroups-win
                tests/run-live-cgroups-win-bench.sh
                BENCHMARK
                DEPENDS netipc-cgroups-live-c netipc-codec-go netipc-codec-rs
            )
            if(TARGET netipc-bench-live-cgroups-win)
                add_dependencies(netipc-bench netipc-bench-live-cgroups-win)
            endif()
        endif()

        netipc_add_script_workflow(
            netipc-bench-negotiated-profile
            tests/run-negotiated-profile-bench.sh
            BENCHMARK
            DEPENDS netipc_live_uds_rs
        )
        if(TARGET netipc-bench-negotiated-profile)
            add_dependencies(netipc-bench netipc-bench-negotiated-profile)
        endif()
    else()
        message(STATUS "bash not found; skipping script workflow registration")
    endif()
endif()
