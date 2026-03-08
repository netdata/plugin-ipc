cmake_minimum_required(VERSION 3.20)

project(plugin_ipc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(NETIPC_BUILD_FIXTURES "Build C fixture binaries" ON)
option(NETIPC_BUILD_BENCH "Build the C benchmark driver" ON)
option(NETIPC_BUILD_HELPERS "Build Go/Rust helper binaries via CMake" ON)

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
