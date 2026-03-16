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

# Coverage build option: adds -fprofile-arcs -ftest-coverage to all C targets
option(NETIPC_COVERAGE "Enable code coverage instrumentation (gcov)" OFF)
if(NETIPC_COVERAGE)
    add_compile_options(-fprofile-arcs -ftest-coverage -O0 -g)
    add_link_options(-fprofile-arcs -ftest-coverage)
endif()

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

# --- C library: L1 Windows Named Pipe transport ----------------------------

if(NETIPC_WINDOWS_RUNTIME)
    add_library(netipc_named_pipe STATIC
        src/libnetdata/netipc/src/transport/windows/netipc_named_pipe.c
    )
    target_include_directories(netipc_named_pipe PUBLIC
        src/libnetdata/netipc/include
    )
    target_link_libraries(netipc_named_pipe PUBLIC netipc_protocol)

    # --- C library: L1 Windows SHM transport --------------------------------

    add_library(netipc_win_shm STATIC
        src/libnetdata/netipc/src/transport/windows/netipc_win_shm.c
    )
    target_include_directories(netipc_win_shm PUBLIC
        src/libnetdata/netipc/include
    )
    target_link_libraries(netipc_win_shm PUBLIC netipc_protocol netipc_named_pipe)

    # --- C library: L2/L3 Windows service orchestration ----------------------

    add_library(netipc_service_win STATIC
        src/libnetdata/netipc/src/service/netipc_service_win.c
    )
    target_include_directories(netipc_service_win PUBLIC
        src/libnetdata/netipc/include
    )
    target_link_libraries(netipc_service_win PUBLIC
        netipc_protocol netipc_named_pipe netipc_win_shm
    )
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

    # Multi-client multi-worker server tests
    add_executable(test_multi_server tests/fixtures/c/test_multi_server.c)
    target_link_libraries(test_multi_server PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )
    add_test(NAME test_multi_server COMMAND test_multi_server)
    set_tests_properties(test_multi_server PROPERTIES TIMEOUT 120)

    # C L2 interop binary
    add_executable(interop_service_c tests/fixtures/c/interop_service.c)
    target_link_libraries(interop_service_c PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )

    # Stress tests (Phase H4: large-scale and stress testing)
    add_executable(test_stress tests/fixtures/c/test_stress.c)
    target_link_libraries(test_stress PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )
    add_test(NAME test_stress COMMAND test_stress)
    set_tests_properties(test_stress PROPERTIES TIMEOUT 180)

    # Ping-pong tests: INCREMENT + STRING_REVERSE L2 typed calls
    add_executable(test_ping_pong tests/fixtures/c/test_ping_pong.c)
    target_link_libraries(test_ping_pong PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )
    add_test(NAME test_ping_pong COMMAND test_ping_pong)
    set_tests_properties(test_ping_pong PROPERTIES TIMEOUT 30)

    # Chaos tests (Phase H9: transport-level and SHM chaos testing)
    add_executable(test_chaos tests/fixtures/c/test_chaos.c)
    target_link_libraries(test_chaos PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )
    add_test(NAME test_chaos COMMAND test_chaos)
    set_tests_properties(test_chaos PROPERTIES TIMEOUT 120)
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
        set_tests_properties(test_service_rust PROPERTIES TIMEOUT 120)

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

# --- L3 cache tests (Linux only) -------------------------------------------

if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
    add_executable(test_cache tests/fixtures/c/test_cache.c)
    target_link_libraries(test_cache PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )
    add_test(NAME test_cache COMMAND test_cache)
    set_tests_properties(test_cache PROPERTIES TIMEOUT 120)

    # C L3 interop binary
    add_executable(interop_cache_c tests/fixtures/c/interop_cache.c)
    target_link_libraries(interop_cache_c PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads
    )
endif()

# --- Rust L3 cache tests ---------------------------------------------------

if(CARGO_EXECUTABLE AND NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
    # Build the Rust L3 interop binary
    add_custom_target(interop_cache_rs ALL
        COMMAND ${CARGO_EXECUTABLE} build --bin interop_cache
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        COMMENT "Building Rust L3 cache interop binary"
    )
endif()

# --- Go L3 cache tests -----------------------------------------------------

if(GO_EXECUTABLE AND NOT NETIPC_WINDOWS_RUNTIME)
    # Go L3 cache tests (in same package as L2)
    add_test(NAME test_cache_go
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} test -count=1 -timeout=120s
            -run TestCache ./pkg/netipc/service/cgroups/
        WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
    )
    set_tests_properties(test_cache_go PROPERTIES TIMEOUT 180)

    # Build Go L3 interop binary
    add_custom_target(interop_cache_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_cache_go" ./cmd/interop_cache
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
        COMMENT "Building Go L3 cache interop binary"
    )
endif()

# --- Go stress tests (Phase H4) ---------------------------------------------

if(GO_EXECUTABLE AND NOT NETIPC_WINDOWS_RUNTIME)
    add_test(NAME test_stress_go
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} test -count=1 -timeout=180s
            -run TestStress ./pkg/netipc/service/cgroups/
        WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
    )
    set_tests_properties(test_stress_go PROPERTIES TIMEOUT 240)
endif()

# --- Rust stress tests (Phase H4) -------------------------------------------

if(CARGO_EXECUTABLE AND NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
    add_test(NAME test_stress_rust
        COMMAND ${CARGO_EXECUTABLE} test test_stress -- --test-threads=1
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
    )
    set_tests_properties(test_stress_rust PROPERTIES TIMEOUT 240)
endif()

# --- Cross-language L2 service SHM interop tests (Linux only) ---------------

if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE AND CARGO_EXECUTABLE)
    add_test(NAME test_service_shm_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_service_shm_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_service_shm_interop PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "INTEROP_SVC_C=$<TARGET_FILE:interop_service_c>;INTEROP_SVC_RS=${INTEROP_SVC_RS};INTEROP_SVC_GO=${INTEROP_SVC_GO}"
    )
endif()

# --- Cross-language L3 cache interop tests (Linux only) ---------------------

if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE AND CARGO_EXECUTABLE)
    set(INTEROP_CACHE_RS "${NETIPC_RUST_CRATE_DIR}/target/debug/interop_cache")
    set(INTEROP_CACHE_GO "${CMAKE_BINARY_DIR}/bin/interop_cache_go")

    add_test(NAME test_cache_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_cache_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_cache_interop PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "INTEROP_CACHE_C=$<TARGET_FILE:interop_cache_c>;INTEROP_CACHE_RS=${INTEROP_CACHE_RS};INTEROP_CACHE_GO=${INTEROP_CACHE_GO}"
    )

    # SHM variant
    add_test(NAME test_cache_shm_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_cache_shm_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_cache_shm_interop PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "INTEROP_CACHE_C=$<TARGET_FILE:interop_cache_c>;INTEROP_CACHE_RS=${INTEROP_CACHE_RS};INTEROP_CACHE_GO=${INTEROP_CACHE_GO}"
    )
endif()

# --- L1 Windows Named Pipe transport tests ---------------------------------

if(NETIPC_WINDOWS_RUNTIME)
    add_executable(test_named_pipe tests/fixtures/c/test_named_pipe.c)
    target_link_libraries(test_named_pipe PRIVATE netipc_named_pipe netipc_protocol)
    add_test(NAME test_named_pipe COMMAND test_named_pipe)
    set_tests_properties(test_named_pipe PROPERTIES TIMEOUT 30)

    # C Named Pipe interop binary
    add_executable(interop_named_pipe_c tests/fixtures/c/interop_named_pipe.c)
    target_link_libraries(interop_named_pipe_c PRIVATE netipc_named_pipe netipc_protocol)
endif()

# --- Rust Named Pipe transport (Windows) -----------------------------------

if(CARGO_EXECUTABLE AND NETIPC_WINDOWS_RUNTIME)
    add_custom_target(interop_named_pipe_rs ALL
        COMMAND ${CARGO_EXECUTABLE} build --bin interop_named_pipe
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        COMMENT "Building Rust Named Pipe interop binary"
    )
endif()

# --- Go Named Pipe transport (Windows) ------------------------------------

if(GO_EXECUTABLE AND NETIPC_WINDOWS_RUNTIME)
    add_test(NAME test_named_pipe_go
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} test -count=1 -timeout=60s
            ./pkg/netipc/transport/windows/
        WORKING_DIRECTORY "${NETIPC_GO_PKG_DIR}"
    )
    set_tests_properties(test_named_pipe_go PROPERTIES TIMEOUT 60)

    # Build Go Named Pipe interop binary
    add_custom_target(interop_named_pipe_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_named_pipe_go.exe"
            ./cmd/interop_named_pipe
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
        COMMENT "Building Go Named Pipe interop binary"
    )
endif()

# --- Cross-language Named Pipe interop tests (Windows) ---------------------

if(NETIPC_WINDOWS_RUNTIME AND CARGO_EXECUTABLE)
    set(INTEROP_NP_RS "${NETIPC_RUST_CRATE_DIR}/target/debug/interop_named_pipe.exe")
    set(INTEROP_NP_GO "${CMAKE_BINARY_DIR}/bin/interop_named_pipe_go.exe")

    add_test(NAME test_named_pipe_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_named_pipe_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_named_pipe_interop PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "INTEROP_NP_C=$<TARGET_FILE:interop_named_pipe_c>;INTEROP_NP_RS=${INTEROP_NP_RS};INTEROP_NP_GO=${INTEROP_NP_GO}"
    )
endif()

# --- L1 Windows SHM transport tests ----------------------------------------

if(NETIPC_WINDOWS_RUNTIME)
    add_executable(test_win_shm tests/fixtures/c/test_win_shm.c)
    target_link_libraries(test_win_shm PRIVATE netipc_win_shm netipc_named_pipe netipc_protocol)
    add_test(NAME test_win_shm COMMAND test_win_shm)
    set_tests_properties(test_win_shm PROPERTIES TIMEOUT 30)

    # C Windows SHM interop binary
    add_executable(interop_win_shm_c tests/fixtures/c/interop_win_shm.c)
    target_link_libraries(interop_win_shm_c PRIVATE netipc_win_shm netipc_named_pipe netipc_protocol)
endif()

# --- Rust Windows SHM transport (Windows) -----------------------------------

if(CARGO_EXECUTABLE AND NETIPC_WINDOWS_RUNTIME)
    add_custom_target(interop_win_shm_rs ALL
        COMMAND ${CARGO_EXECUTABLE} build --bin interop_win_shm
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        COMMENT "Building Rust Windows SHM interop binary"
    )
endif()

# --- Go Windows SHM transport (Windows) ------------------------------------

if(GO_EXECUTABLE AND NETIPC_WINDOWS_RUNTIME)
    # Build Go Windows SHM interop binary
    add_custom_target(interop_win_shm_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_win_shm_go.exe"
            ./cmd/interop_win_shm
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
        COMMENT "Building Go Windows SHM interop binary"
    )
endif()

# --- Cross-language Windows SHM interop tests (Windows) ---------------------

if(NETIPC_WINDOWS_RUNTIME AND CARGO_EXECUTABLE)
    set(INTEROP_WIN_SHM_RS "${NETIPC_RUST_CRATE_DIR}/target/debug/interop_win_shm.exe")
    set(INTEROP_WIN_SHM_GO "${CMAKE_BINARY_DIR}/bin/interop_win_shm_go.exe")

    add_test(NAME test_win_shm_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_win_shm_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_win_shm_interop PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "INTEROP_WIN_SHM_C=$<TARGET_FILE:interop_win_shm_c>;INTEROP_WIN_SHM_RS=${INTEROP_WIN_SHM_RS};INTEROP_WIN_SHM_GO=${INTEROP_WIN_SHM_GO}"
    )
endif()

# --- Windows L2 service interop binaries -----------------------------------

if(NETIPC_WINDOWS_RUNTIME)
    # C Windows L2 interop binary
    add_executable(interop_service_win_c tests/fixtures/c/interop_service_win.c)
    target_link_libraries(interop_service_win_c PRIVATE
        netipc_service_win netipc_win_shm netipc_named_pipe netipc_protocol
    )

    # C Windows L3 cache interop binary
    add_executable(interop_cache_win_c tests/fixtures/c/interop_cache_win.c)
    target_link_libraries(interop_cache_win_c PRIVATE
        netipc_service_win netipc_win_shm netipc_named_pipe netipc_protocol
    )
endif()

# --- Rust Windows L2/L3 interop binaries ------------------------------------

if(CARGO_EXECUTABLE AND NETIPC_WINDOWS_RUNTIME)
    add_custom_target(interop_service_win_rs ALL
        COMMAND ${CARGO_EXECUTABLE} build --bin interop_service_win
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        COMMENT "Building Rust Windows L2 service interop binary"
    )

    add_custom_target(interop_cache_win_rs ALL
        COMMAND ${CARGO_EXECUTABLE} build --bin interop_cache_win
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        COMMENT "Building Rust Windows L3 cache interop binary"
    )
endif()

# --- Go Windows L2/L3 interop binaries -------------------------------------

if(GO_EXECUTABLE AND NETIPC_WINDOWS_RUNTIME)
    add_custom_target(interop_service_win_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_service_win_go.exe"
            ./cmd/interop_service_win
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
        COMMENT "Building Go Windows L2 service interop binary"
    )

    add_custom_target(interop_cache_win_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/interop_cache_win_go.exe"
            ./cmd/interop_cache_win
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tests/fixtures/go"
        COMMENT "Building Go Windows L3 cache interop binary"
    )
endif()

# --- Cross-language Windows L2 service interop tests ------------------------

if(NETIPC_WINDOWS_RUNTIME AND CARGO_EXECUTABLE)
    set(INTEROP_SVC_WIN_RS "${NETIPC_RUST_CRATE_DIR}/target/debug/interop_service_win.exe")
    set(INTEROP_SVC_WIN_GO "${CMAKE_BINARY_DIR}/bin/interop_service_win_go.exe")

    add_test(NAME test_service_win_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_service_win_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_service_win_interop PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "INTEROP_SVC_C=$<TARGET_FILE:interop_service_win_c>;INTEROP_SVC_RS=${INTEROP_SVC_WIN_RS};INTEROP_SVC_GO=${INTEROP_SVC_WIN_GO}"
    )

    # SHM variant
    add_test(NAME test_service_win_shm_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_service_win_shm_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_service_win_shm_interop PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "INTEROP_SVC_C=$<TARGET_FILE:interop_service_win_c>;INTEROP_SVC_RS=${INTEROP_SVC_WIN_RS};INTEROP_SVC_GO=${INTEROP_SVC_WIN_GO}"
    )
endif()

# --- Cross-language Windows L3 cache interop tests --------------------------

if(NETIPC_WINDOWS_RUNTIME AND CARGO_EXECUTABLE)
    set(INTEROP_CACHE_WIN_RS "${NETIPC_RUST_CRATE_DIR}/target/debug/interop_cache_win.exe")
    set(INTEROP_CACHE_WIN_GO "${CMAKE_BINARY_DIR}/bin/interop_cache_win_go.exe")

    add_test(NAME test_cache_win_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_cache_win_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_cache_win_interop PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "INTEROP_CACHE_C=$<TARGET_FILE:interop_cache_win_c>;INTEROP_CACHE_RS=${INTEROP_CACHE_WIN_RS};INTEROP_CACHE_GO=${INTEROP_CACHE_WIN_GO}"
    )

    # SHM variant
    add_test(NAME test_cache_win_shm_interop
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_cache_win_shm_interop.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(test_cache_win_shm_interop PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "INTEROP_CACHE_C=$<TARGET_FILE:interop_cache_win_c>;INTEROP_CACHE_RS=${INTEROP_CACHE_WIN_RS};INTEROP_CACHE_GO=${INTEROP_CACHE_WIN_GO}"
    )
endif()

# --- Benchmarks (Phase 13) ------------------------------------------------

if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
    # C benchmark binary
    add_executable(bench_posix_c bench/drivers/c/bench_posix.c)
    target_link_libraries(bench_posix_c PRIVATE
        netipc_service netipc_shm netipc_uds netipc_protocol Threads::Threads m
    )
    target_compile_options(bench_posix_c PRIVATE -O2)
endif()

# Rust benchmark binary (release build for accurate performance)
if(CARGO_EXECUTABLE AND NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
    add_custom_target(bench_posix_rs
        COMMAND ${CARGO_EXECUTABLE} build --release --bin bench_posix
        WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
        COMMENT "Building Rust benchmark binary (release)"
    )
endif()

# Go benchmark binary
if(GO_EXECUTABLE AND NOT NETIPC_WINDOWS_RUNTIME)
    add_custom_target(bench_posix_go ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
            ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/bench_posix_go" .
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/bench/drivers/go"
        COMMENT "Building Go benchmark binary"
    )
endif()

# Benchmark runner (not CTest — too slow for CI)
if(NOT NETIPC_WINDOWS_RUNTIME AND NOT APPLE)
    add_custom_target(run-posix-bench
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run-posix-bench.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running full POSIX benchmark suite"
        DEPENDS bench_posix_c
    )

    add_custom_target(generate-benchmarks-posix
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/generate-benchmarks-posix.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating benchmarks-posix.md from CSV data"
    )
endif()

# --- Windows Benchmarks ---------------------------------------------------

if(NETIPC_WINDOWS_RUNTIME)
    # C Windows benchmark binary
    add_executable(bench_windows_c bench/drivers/c/bench_windows.c)
    target_link_libraries(bench_windows_c PRIVATE
        netipc_service_win netipc_named_pipe netipc_win_shm netipc_protocol
    )
    target_compile_options(bench_windows_c PRIVATE -O2)

    # Rust Windows benchmark binary (release build for accurate performance)
    if(CARGO_EXECUTABLE)
        add_custom_target(bench_windows_rs
            COMMAND ${CARGO_EXECUTABLE} build --release --bin bench_windows
            WORKING_DIRECTORY "${NETIPC_RUST_CRATE_DIR}"
            COMMENT "Building Rust Windows benchmark binary (release)"
        )
    endif()

    # Go Windows benchmark binary
    if(GO_EXECUTABLE)
        add_custom_target(bench_windows_go ALL
            COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0
                ${GO_EXECUTABLE} build -o "${CMAKE_BINARY_DIR}/bin/bench_windows_go.exe" .
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/bench/drivers/go"
            COMMENT "Building Go Windows benchmark binary"
        )
    endif()

    add_custom_target(run-windows-bench
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/run-windows-bench.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Running full Windows benchmark suite"
        DEPENDS bench_windows_c
    )

    add_custom_target(generate-benchmarks-windows
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/generate-benchmarks-windows.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating benchmarks-windows.md from CSV data"
    )
endif()
