set(NETIPC_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(NETIPC_LIBRARY_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/protocol/netipc_schema.c"
)

if(NETIPC_WINDOWS_RUNTIME)
    list(APPEND NETIPC_LIBRARY_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/windows/netipc_shm_hybrid_win.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/windows/netipc_named_pipe.c"
    )
else()
    list(APPEND NETIPC_LIBRARY_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/posix/netipc_shm_hybrid.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/posix/netipc_uds_seqpacket.c"
    )
endif()

add_library(netipc STATIC ${NETIPC_LIBRARY_SOURCES})
target_include_directories(netipc PUBLIC "${NETIPC_PUBLIC_INCLUDE_DIR}")
target_link_libraries(netipc PUBLIC Threads::Threads)
if(NETIPC_WINDOWS_RUNTIME)
    target_link_libraries(netipc PUBLIC synchronization)
endif()

if(NETIPC_BUILD_FIXTURES)
    add_executable(netipc-codec-c "${CMAKE_SOURCE_DIR}/tests/fixtures/c/netipc_codec_tool.c")
    target_link_libraries(netipc-codec-c PRIVATE netipc)

    if(NETIPC_WINDOWS_RUNTIME)
        add_executable(netipc-live-c "${CMAKE_SOURCE_DIR}/tests/fixtures/c/netipc_live_c.c")
        target_link_libraries(netipc-live-c PRIVATE netipc winmm)

        add_executable(shm-raw-bench "${CMAKE_SOURCE_DIR}/tests/fixtures/c/shm_raw_bench.c")
        target_link_libraries(shm-raw-bench PRIVATE Threads::Threads)

        add_executable(shm-bulk-bench "${CMAKE_SOURCE_DIR}/tests/fixtures/c/shm_bulk_bench.c")
        target_link_libraries(shm-bulk-bench PRIVATE Threads::Threads)
    else()
        add_executable(netipc-live-c "${CMAKE_SOURCE_DIR}/tests/fixtures/c/netipc_live_posix_c.c")
        target_link_libraries(netipc-live-c PRIVATE netipc)
    endif()
endif()
