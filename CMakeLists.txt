cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_WIN32 AND DEFINED ENV{MSYSTEM} AND "$ENV{MSYSTEM}" STREQUAL "MSYS")
    message(FATAL_ERROR
        "On Windows, build plugin-ipc from an MSYS2 MinGW/UCRT shell "
        "(`mingw64.exe` or `ucrt64.exe`), not the plain `msys` shell. "
        "The Windows C backend is native Win32/Named Pipes, not an MSYS runtime build."
    )
endif()

project(plugin_ipc LANGUAGES C)

include(CTest)

if(WIN32)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
            RESULT_VARIABLE NETIPC_C_COMPILER_TARGET_RC
            OUTPUT_VARIABLE NETIPC_C_COMPILER_TARGET
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NETIPC_C_COMPILER_TARGET_RC EQUAL 0 AND
           NETIPC_C_COMPILER_TARGET MATCHES "(msys|cygwin)")
            message(FATAL_ERROR
                "Compiler target `${NETIPC_C_COMPILER_TARGET}` is not a native Windows target. "
                "Use an MSYS2 MinGW/UCRT toolchain such as `mingw64` or `ucrt64`."
            )
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
    set(NETIPC_CODEC_RS_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc-codec-rs")
    set(NETIPC_LIVE_RS_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc_live_rs")

    if(CARGO_EXECUTABLE)
        add_custom_command(
            OUTPUT "${NETIPC_CODEC_RS_BIN}" "${NETIPC_LIVE_RS_BIN}"
            COMMAND "${CARGO_EXECUTABLE}" build --release --manifest-path "${NETIPC_RUST_FIXTURES_MANIFEST}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${NETIPC_RUST_FIXTURES_TARGET}/netipc-codec-rs"
                    "${NETIPC_CODEC_RS_BIN}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${NETIPC_RUST_FIXTURES_TARGET}/netipc_live_rs"
                    "${NETIPC_LIVE_RS_BIN}"
            DEPENDS
                "${NETIPC_RUST_FIXTURES_MANIFEST}"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/build.rs"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/src/bin/netipc_codec_rs.rs"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/rust/src/bin/netipc_live_rs.rs"
                ${NETIPC_RUST_CRATE_SOURCES}
            VERBATIM
        )

        add_custom_target(netipc-codec-rs DEPENDS "${NETIPC_CODEC_RS_BIN}")
        add_custom_target(netipc_live_rs DEPENDS "${NETIPC_LIVE_RS_BIN}")

        set(NETIPC_RUST_BENCH_MANIFEST "${CMAKE_SOURCE_DIR}/bench/drivers/rust/Cargo.toml")
        set(NETIPC_RUST_BENCH_TARGET "${CMAKE_SOURCE_DIR}/bench/drivers/rust/target/release")
        set(NETIPC_LIVE_UDS_RS_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc_live_uds_rs")

        add_custom_command(
            OUTPUT "${NETIPC_LIVE_UDS_RS_BIN}"
            COMMAND "${CARGO_EXECUTABLE}" build --release --manifest-path "${NETIPC_RUST_BENCH_MANIFEST}"
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
    endif()

    if(GO_EXECUTABLE)
        set(NETIPC_CODEC_GO_BIN "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/netipc-codec-go")
        add_custom_command(
            OUTPUT "${NETIPC_CODEC_GO_BIN}"
            COMMAND "${CMAKE_COMMAND}" -E chdir "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
                    "${GO_EXECUTABLE}" build -o "${NETIPC_CODEC_GO_BIN}" .
            DEPENDS
                "${CMAKE_SOURCE_DIR}/tests/fixtures/go/go.mod"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/go/main.go"
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

        if(WIN32)
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
        endif()

        netipc_add_script_workflow(
            netipc-bench-live-shm
            tests/run-live-shm-bench.sh
            BENCHMARK
            DEPENDS netipc-live-c
        )
        if(TARGET netipc-bench-live-shm)
            add_dependencies(netipc-bench netipc-bench-live-shm)
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
