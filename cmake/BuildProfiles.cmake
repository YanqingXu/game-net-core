# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0

set(GAMENET_PGO_PHASE "OFF" CACHE STRING "PGO phase: OFF, GENERATE, or USE")
set_property(CACHE GAMENET_PGO_PHASE PROPERTY STRINGS OFF GENERATE USE)
set(
    GAMENET_PGO_DATA_DIR
    "${CMAKE_BINARY_DIR}/pgo-data"
    CACHE PATH
    "Compiler profile data directory for GCC/Clang PGO"
)

string(TOUPPER "${GAMENET_PGO_PHASE}" GAMENET_PGO_PHASE_NORMALIZED)
if(NOT GAMENET_PGO_PHASE_NORMALIZED STREQUAL "OFF"
   AND NOT GAMENET_PGO_PHASE_NORMALIZED STREQUAL "GENERATE"
   AND NOT GAMENET_PGO_PHASE_NORMALIZED STREQUAL "USE")
    message(FATAL_ERROR "GAMENET_PGO_PHASE must be OFF, GENERATE, or USE")
endif()

if(GAMENET_ENABLE_NATIVE_TUNING)
    if(MSVC)
        add_compile_options(/arch:AVX2)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-march=native)
    else()
        message(FATAL_ERROR "native tuning is unsupported for ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(GAMENET_ENABLE_BENCHMARK_INSTRUMENTATION)
    if(MSVC)
        add_compile_options(/Oy- /Zi)
        add_link_options(/DEBUG)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fno-omit-frame-pointer -g1)
    else()
        message(FATAL_ERROR "benchmark instrumentation is unsupported for ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(NOT GAMENET_PGO_PHASE_NORMALIZED STREQUAL "OFF")
    if(GAMENET_ENABLE_ASAN_UBSAN OR GAMENET_ENABLE_TSAN)
        message(FATAL_ERROR "PGO cannot be combined with sanitizer instrumentation")
    endif()
    if(MSVC)
        add_compile_options(/GL)
        if(GAMENET_PGO_PHASE_NORMALIZED STREQUAL "GENERATE")
            add_link_options(/LTCG:PGINSTRUMENT)
        else()
            add_link_options(/LTCG:PGOPTIMIZE)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        if(GAMENET_PGO_PHASE_NORMALIZED STREQUAL "GENERATE")
            add_compile_options("-fprofile-generate=${GAMENET_PGO_DATA_DIR}")
            add_link_options("-fprofile-generate=${GAMENET_PGO_DATA_DIR}")
        else()
            add_compile_options(
                "-fprofile-use=${GAMENET_PGO_DATA_DIR}"
                -fprofile-correction
            )
            add_link_options(
                "-fprofile-use=${GAMENET_PGO_DATA_DIR}"
                -fprofile-correction
            )
        endif()
    else()
        message(FATAL_ERROR "PGO is unsupported for ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()
