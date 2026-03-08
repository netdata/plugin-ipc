set(NETIPC_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(NETIPC_LIBRARY_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/protocol/netipc_schema.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/posix/netipc_shm_hybrid.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/posix/netipc_uds_seqpacket.c"
)

add_library(netipc STATIC ${NETIPC_LIBRARY_SOURCES})
target_include_directories(netipc PUBLIC "${NETIPC_PUBLIC_INCLUDE_DIR}")
target_link_libraries(netipc PUBLIC Threads::Threads)

if(NETIPC_BUILD_FIXTURES)
    add_executable(netipc-codec-c "${CMAKE_SOURCE_DIR}/tests/fixtures/c/netipc_codec_tool.c")
    target_link_libraries(netipc-codec-c PRIVATE netipc)

    add_executable(netipc-shm-server-demo "${CMAKE_SOURCE_DIR}/tests/fixtures/c/netipc_shm_server_demo.c")
    target_link_libraries(netipc-shm-server-demo PRIVATE netipc)

    add_executable(netipc-shm-client-demo "${CMAKE_SOURCE_DIR}/tests/fixtures/c/netipc_shm_client_demo.c")
    target_link_libraries(netipc-shm-client-demo PRIVATE netipc)

    add_executable(netipc-uds-server-demo "${CMAKE_SOURCE_DIR}/tests/fixtures/c/netipc_uds_server_demo.c")
    target_link_libraries(netipc-uds-server-demo PRIVATE netipc)

    add_executable(netipc-uds-client-demo "${CMAKE_SOURCE_DIR}/tests/fixtures/c/netipc_uds_client_demo.c")
    target_link_libraries(netipc-uds-client-demo PRIVATE netipc)
endif()

if(NETIPC_BUILD_BENCH)
    add_executable(ipc-bench "${CMAKE_SOURCE_DIR}/bench/drivers/c/ipc_bench.c")
    target_include_directories(ipc-bench PRIVATE "${NETIPC_PUBLIC_INCLUDE_DIR}")
    target_link_libraries(ipc-bench PRIVATE Threads::Threads m)
endif()
