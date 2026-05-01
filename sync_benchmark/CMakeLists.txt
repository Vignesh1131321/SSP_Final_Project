cmake_minimum_required(VERSION 3.16)
project(sync_benchmark CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags ───────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -fsanitize=thread)
    add_link_options(-fsanitize=thread)
else()
    add_compile_options(-O3 -march=native -DNDEBUG)
endif()

# ── Threads ──────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Sources ──────────────────────────────────────────────────
set(SOURCES
    main.cpp
    locks/lock_dispatch.cpp
    experiments/scalability.cpp
    experiments/contention.cpp
    experiments/false_sharing.cpp
    experiments/fairness.cpp
    experiments/latency_dist.cpp
    experiments/throughput_saturation.cpp
)

add_executable(sync_bench ${SOURCES})
target_link_libraries(sync_bench PRIVATE Threads::Threads)

# perf_event on Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(sync_bench PRIVATE __linux__)
endif()

# ── Output directory ─────────────────────────────────────────
add_custom_command(TARGET sync_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory output
    COMMENT "Creating output directory"
)

# ── Install ──────────────────────────────────────────────────
install(TARGETS sync_bench DESTINATION bin)
