cmake_minimum_required(VERSION 3.16)
project(sb_tracer C)

set(SAMEBOY_DIR "D:/SameBoy")

# Collect all SameBoy Core sources
file(GLOB SAMEBOY_CORE_SRCS "${SAMEBOY_DIR}/Core/*.c")

add_executable(sb_tracer
    sb_tracer.c
    ${SAMEBOY_CORE_SRCS}
)

target_include_directories(sb_tracer PRIVATE
    ${SAMEBOY_DIR}
)

target_compile_definitions(sb_tracer PRIVATE
    GB_INTERNAL
    GB_DISABLE_CHEATS
    GB_DISABLE_DEBUGGER
    GB_DISABLE_REWIND
    GB_DISABLE_CHEAT_SEARCH
    SAMEBOY_CORE_ONLY
    _GNU_SOURCE
    GB_VERSION="1.0.3"
)

# Remove files that won't compile without their dependencies
# (cheat_search, cheats, debugger, rewind are disabled via defines but may still be in glob)
# Filter them out
set(FILTERED_SRCS)
foreach(src ${SAMEBOY_CORE_SRCS})
    get_filename_component(name ${src} NAME)
    if(name STREQUAL "debugger.c" OR name STREQUAL "cheat_search.c" OR name STREQUAL "cheats.c" OR name STREQUAL "rewind.c" OR name STREQUAL "symbol_hash.c" OR name STREQUAL "sm83_disassembler.c")
        # Skip
    else()
        list(APPEND FILTERED_SRCS ${src})
    endif()
endforeach()

# Rebuild with filtered sources
set_target_properties(sb_tracer PROPERTIES SOURCES "")
target_sources(sb_tracer PRIVATE
    sb_tracer.c
    ${FILTERED_SRCS}
)

target_compile_options(sb_tracer PRIVATE -O2 -Wall -Wno-unused-function -Wno-multichar)

if(WIN32)
    target_link_libraries(sb_tracer PRIVATE ws2_32)
endif()
