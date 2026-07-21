cmake_minimum_required(VERSION 3.20)

project(GameNetCore VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GAMENET_BUILD_TESTING "Build tests" ON)
option(GAMENET_BUILD_BENCHMARKS "Build opt-in performance benchmarks" OFF)
option(GAMENET_BUILD_FUZZING "Build optional libFuzzer targets" OFF)
option(GAMENET_ENABLE_ASAN_UBSAN "Enable ASan/UBSan" OFF)
option(GAMENET_ENABLE_TSAN "Enable TSan" OFF)
option(GAMENET_ENABLE_TLS "Build TLS support" OFF)
option(GAMENET_ENABLE_EXPERIMENTAL "Build experimental modules" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(Sanitizers)

set(GAMENET_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/GameNetCore"
    CACHE STRING "Install destination for GameNetCore CMake package files"
)

add_subdirectory(src/core)
add_subdirectory(src/protocol)
add_subdirectory(src/transport)
add_subdirectory(src/game_session)
add_subdirectory(src/game_logic)
add_subdirectory(src/broadcast)
add_subdirectory(examples)

if(GAMENET_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(GAMENET_BUILD_FUZZING)
    add_subdirectory(tests/fuzz)
endif()

if(GAMENET_BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

install(DIRECTORY include/gamenet/core
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet
)

install(DIRECTORY include/gamenet/protocol
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet
)

install(DIRECTORY include/gamenet/transport
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet
)

install(DIRECTORY include/gamenet/game_session
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet
)

install(DIRECTORY include/gamenet/game_logic
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet
)

install(DIRECTORY include/gamenet/broadcast
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet
)

configure_package_config_file(
    cmake/GameNetCoreConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/GameNetCoreConfig.cmake"
    INSTALL_DESTINATION ${GAMENET_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/GameNetCoreConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/GameNetCoreConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/GameNetCoreConfigVersion.cmake"
    DESTINATION ${GAMENET_INSTALL_CMAKEDIR}
)

install(EXPORT GameNetCoreTargets
    NAMESPACE GameNet::
    DESTINATION ${GAMENET_INSTALL_CMAKEDIR}
)
