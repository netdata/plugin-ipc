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

    # --- C library: L1 POSIX SHM transport (Linux only) ----------------------

    if(NOT APPLE)
        add_library(netipc_shm STATIC
            src/libnetdata/netipc/src/transport/posix/netipc_shm.c
        )
        target_include_directories(netipc_shm PUBLIC
            src/libnetdata/netipc/include
        )
        target_link_libraries(netipc_shm PUBLIC netipc_protocol)

        # --- C library: L2 service orchestration (Linux only, needs SHM+UDS) ----

        add_library(netipc_service STATIC
            src/libnetdata/netipc/src/service/netipc_service.c
        )
        target_include_directories(netipc_service PUBLIC
            src/libnetdata/netipc/include
        )
        target_link_libraries(netipc_service PUBLIC
            netipc_protocol netipc_uds netipc_shm Threads::Threads
        )
    endif()
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
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_codec_go" ./cmd/interop_codec
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
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

    # C interop binary for cross-language UDS tests
    add_executable(interop_uds_c tests/fixtures/c/interop_uds.c)
    target_link_libraries(interop_uds_c PRIVATE netipc_uds netipc_protocol)
endif()

# --- L1 SHM transport tests (Linux only) -----------------------------------

if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
    add_executable(test_shm tests/fixtures/c/test_shm.c)
    target_link_libraries(test_shm PRIVATE netipc_shm netipc_uds netipc_protocol Threads::Threads)
    add_test(NAME test_shm COMMAND test_shm)
    set_tests_properties(test_shm PROPERTIES TIMEOUT 30)

    # C SHM interop binary
    add_executable(interop_shm_c tests/fixtures/c/interop_shm.c)
    target_link_libraries(interop_shm_c PRIVATE netipc_shm netipc_protocol)
endif()

# --- L2 service orchestration tests (Linux only) ---------------------------

if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
    add_executable(test_service tests/fixtures/c/test_service.c)
    target_link_libraries(test_service PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )
    add_test(NAME test_service COMMAND test_service)
    set_tests_properties(test_service PROPERTIES TIMEOUT 60)

    # C L2 interop binary
    add_executable(interop_service_c tests/fixtures/c/interop_service.c)
    target_link_libraries(interop_service_c PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )
endif()

# --- Rust UDS transport tests ---------------------------------------------

if(CARGO_EXECUTABLE)
    add_test(NAME test_uds_rust
        COMMAND ${CARGO_EXECUTABLE} test transport -- --test-threads=1
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
    )
    set_tests_properties(test_uds_rust PROPERTIES TIMEOUT 30)

    # Build the Rust interop binary
    add_custom_target(interop_uds_rs ALL
        COMMAND ${CARGO_EXECUTABLE} build --bin interop_uds
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        COMMENT "Building Rust UDS interop binary"
    )

    # Rust SHM transport tests (Linux only)
    if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
        add_test(NAME test_shm_rust
            COMMAND ${CARGO_EXECUTABLE} test shm -- --test-threads=1
            WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        )
        set_tests_properties(test_shm_rust PROPERTIES TIMEOUT 30)

        # Build the Rust SHM interop binary
        add_custom_target(interop_shm_rs ALL
            COMMAND ${CARGO_EXECUTABLE} build --bin interop_shm
            WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
            COMMENT "Building Rust SHM interop binary"
        )

        # Rust L2 service tests
        add_test(NAME test_service_rust
            COMMAND ${CARGO_EXECUTABLE} test service -- --test-threads=1
            WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        )
        set_tests_properties(test_service_rust PROPERTIES TIMEOUT 60)

        # Build the Rust L2 interop binary
        add_custom_target(interop_service_rs ALL
            COMMAND ${CARGO_EXECUTABLE} build --bin interop_service
            WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
            COMMENT "Building Rust L2 service interop binary"
        )
    endif()
endif()

# --- Go UDS transport tests ------------------------------------------------

if(GO_EXECUTABLE AND NOT NETIPC_WINDOWS_RUNTIME)
    add_test(NAME test_uds_go
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} test -count=1 -timeout=60s ./pkg/netipc/transport/posix/
        WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
    )
    set_tests_properties(test_uds_go PROPERTIES TIMEOUT 60)

    # Build Go UDS interop binary
    add_custom_target(interop_uds_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_uds_go" ./cmd/interop_uds
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
        COMMENT "Building Go UDS interop binary"
    )

    # Go SHM transport tests (Linux only)
    if(NOT APPLE)
        add_test(NAME test_shm_go
            COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
                ${GO_EXECUTABLE} test -count=1 -timeout=60s
                -run TestShm ./pkg/netipc/transport/posix/
            WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
        )
        set_tests_properties(test_shm_go PROPERTIES TIMEOUT 60)

        # Build Go SHM interop binary
        add_custom_target(interop_shm_go ALL
            COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
                ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_shm_go" ./cmd/interop_shm
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
            COMMENT "Building Go SHM interop binary"
        )
    endif()

    # Go L2 service tests
    add_test(NAME test_service_go
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} test -count=1 -timeout=120s
            ./pkg/netipc/service/cgroups/
        WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
    )
    set_tests_properties(test_service_go PROPERTIES TIMEOUT 180)

    # Build Go L2 interop binary
    add_custom_target(interop_service_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_service_go" ./cmd/interop_service
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
        COMMENT "Building Go L2 service interop binary"
    )
endif()

# --- Cross-language UDS interop tests -------------------------------------

if(NOT NETIPC_WINDOWS_RUNTIME AND CARGO_EXECUTABLE)
    set(INTEROP_UDS_RS "${NETIPC_RUST_CRATE_DIR}/target/debug/interop_uds")
    set(INTEROP_UDS_GO "${CMAKE_BINARY_DIR}/bin/interop_uds_go")

    add_test(NAME test_uds_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_uds_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_uds_interop PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "INTEROP_UDS_C=$<TARGET_FILE:interop_uds_c>;INTEROP_UDS_RS=${INTEROP_UDS_RS};INTEROP_UDS_GO=${INTEROP_UDS_GO}"
    )
endif()

# --- Cross-language SHM interop tests (Linux only) -------------------------

if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE AND CARGO_EXECUTABLE)
    set(INTEROP_SHM_RS "${NETIPC_RUST_CRATE_DIR}/target/debug/interop_shm")
    set(INTEROP_SHM_GO "${CMAKE_BINARY_DIR}/bin/interop_shm_go")

    add_test(NAME test_shm_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_shm_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_shm_interop PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "INTEROP_SHM_C=$<TARGET_FILE:interop_shm_c>;INTEROP_SHM_RS=${INTEROP_SHM_RS};INTEROP_SHM_GO=${INTEROP_SHM_GO}"
    )
endif()

# --- Cross-language L2 service interop tests (Linux only) ------------------

if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE AND CARGO_EXECUTABLE)
    set(INTEROP_SVC_RS "${NETIPC_RUST_CRATE_DIR}/target/debug/interop_service")
    set(INTEROP_SVC_GO "${CMAKE_BINARY_DIR}/bin/interop_service_go")

    add_test(NAME test_service_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_service_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_service_interop PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "INTEROP_SVC_C=$<TARGET_FILE:interop_service_c>;INTEROP_SVC_RS=${INTEROP_SVC_RS};INTEROP_SVC_GO=${INTEROP_SVC_GO}"
    )
endif()

# --- Benchmarks (added per phase) ----------------------------------------
