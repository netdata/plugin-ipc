cmake_minimum_required(VERSION 3.20)
project(plugin_ipc LANGUAGES C)

# Default to RelWithDebInfo for single-config generators
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

include(CTest)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Threads REQUIRED)

# Detect Windows runtime (native or MSYS/Cygwin)
set(NETIPC_WINDOWS_RUNTIME OFF)
if(WIN32 OR CYGWIN OR CMAKE_SYSTEM_NAME STREQUAL "MSYS")
    set(NETIPC_WINDOWS_RUNTIME ON)
endif()

# --- C library: protocol (Codec + L1 wire envelope) ----------------------

add_library(netipc_protocol STATIC
    src/libnetdata/netipc/src/protocol/netipc_protocol.c
)
target_include_directories(netipc_protocol PUBLIC
    src/libnetdata/netipc/include
)

# --- C library: L1 POSIX UDS transport ------------------------------------

if(NOT NETIPC_WINDOWS_RUNTIME)
    add_library(netipc_uds STATIC
        src/libnetdata/netipc/src/transport/posix/netipc_uds.c
    )
    target_include_directories(netipc_uds PUBLIC
        src/libnetdata/netipc/include
    )
    target_link_libraries(netipc_uds PUBLIC netipc_protocol)
endif()

# --- Rust crate -----------------------------------------------------------
find_program(CARGO_EXECUTABLE cargo)

if(CARGO_EXECUTABLE)
    set(NETIPC_RUST_CRATE_DIR "${CMAKE_SOURCE_DIR}/src/crates/netipc")

    add_custom_target(netipc_rust ALL
        COMMAND ${CARGO_EXECUTABLE} build
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        COMMENT "Building Rust netipc crate"
    )
endif()

# --- Go package -----------------------------------------------------------
find_program(GO_EXECUTABLE go)

if(GO_EXECUTABLE)
    set(NETIPC_GO_PKG_DIR "${CMAKE_SOURCE_DIR}/src/go")
    set(NETIPC_GO_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/tests/fixtures/go")

    add_custom_target(netipc_go_test
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} test ./pkg/netipc/protocol/
        WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
        COMMENT "Running Go netipc protocol tests"
    )

    add_custom_target(interop_codec_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_codec_go" .
        WORKING_DIRECTORY "${NETIPC_GO_FIXTURE_DIR}"
        COMMENT "Building Go interop codec binary"
    )
endif()

# --- Tests ----------------------------------------------------------------
enable_testing()

add_executable(test_protocol tests/test_protocol.c)
target_link_libraries(test_protocol PRIVATE netipc_protocol)
add_test(NAME test_protocol COMMAND test_protocol)

add_executable(interop_codec_c tests/fixtures/c/interop_codec.c)
target_link_libraries(interop_codec_c PRIVATE netipc_protocol)

if(CARGO_EXECUTABLE)
    add_test(NAME test_protocol_rust
        COMMAND ${CARGO_EXECUTABLE} test
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
    )
endif()

if(GO_EXECUTABLE)
    add_test(NAME test_protocol_go
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} test ./pkg/netipc/protocol/
        WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
    )
endif()

if(CARGO_EXECUTABLE AND GO_EXECUTABLE)
    add_test(NAME interop_codec
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/interop_codec.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
elseif(CARGO_EXECUTABLE)
    add_test(NAME interop_codec
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/interop_codec.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()

# --- Fuzz harness (standalone mode for CI) --------------------------------

add_executable(fuzz_protocol tests/fuzz_protocol.c)
target_compile_definitions(fuzz_protocol PRIVATE FUZZ_STANDALONE)
target_link_libraries(fuzz_protocol PRIVATE netipc_protocol)

# CTest fuzz targets: short 30-second runs with random input via /dev/urandom.
# Each test generates random data and feeds it to the standalone harness.
add_test(NAME fuzz_protocol_30s
    COMMAND bash -c "head -c 65536 /dev/urandom | $<TARGET_FILE:fuzz_protocol>"
)
set_tests_properties(fuzz_protocol_30s PROPERTIES TIMEOUT 60)

if(GO_EXECUTABLE)
    # Go fuzz targets: 30-second runs each.
    foreach(FUZZ_TARGET
        FuzzDecodeHeader
        FuzzDecodeChunkHeader
        FuzzDecodeHello
        FuzzDecodeHelloAck
        FuzzDecodeCgroupsRequest
        FuzzDecodeCgroupsResponse
        FuzzBatchDirDecode
        FuzzBatchItemGet
    )
        add_test(NAME go_${FUZZ_TARGET}
            COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
                ${GO_EXECUTABLE} test -fuzz=^${FUZZ_TARGET}$ -fuzztime=30s
                ./pkg/netipc/protocol/
            WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
        )
        set_tests_properties(go_${FUZZ_TARGET} PROPERTIES TIMEOUT 120)
    endforeach()
endif()

# --- L1 UDS transport tests -----------------------------------------------

if(NOT NETIPC_WINDOWS_RUNTIME)
    add_executable(test_uds tests/fixtures/c/test_uds.c)
    target_link_libraries(test_uds PRIVATE netipc_uds netipc_protocol Threads::Threads)
    add_test(NAME test_uds COMMAND test_uds)
    set_tests_properties(test_uds PROPERTIES TIMEOUT 30)
endif()

# --- Benchmarks (added per phase) ----------------------------------------
