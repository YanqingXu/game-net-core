cmake_minimum_required(VERSION 3.20)

project(GameNetInstallConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(GameNetCore 0.3.0 EXACT REQUIRED)

add_executable(gamenet_install_consumer main.cpp)
target_link_libraries(gamenet_install_consumer
    PRIVATE
        GameNet::core
)

add_executable(gamenet_provisional_install_consumer provisional_main.cpp)
target_link_libraries(gamenet_provisional_install_consumer
    PRIVATE
        GameNet::protocol
        GameNet::transport
        GameNet::game_session
        GameNet::game_logic
        GameNet::broadcast
)

enable_testing()
add_test(NAME gamenet.install_consumer COMMAND gamenet_install_consumer)
add_test(
    NAME gamenet.provisional_install_consumer
    COMMAND gamenet_provisional_install_consumer
)

function(assert_gamenet_version request expected_compatible)
    string(REPLACE "." ";" request_components "${request}")
    list(GET request_components 0 PACKAGE_FIND_VERSION_MAJOR)
    list(GET request_components 1 PACKAGE_FIND_VERSION_MINOR)
    list(GET request_components 2 PACKAGE_FIND_VERSION_PATCH)
    set(PACKAGE_FIND_VERSION "${request}")
    unset(PACKAGE_FIND_VERSION_RANGE)
    unset(PACKAGE_VERSION_COMPATIBLE)
    unset(PACKAGE_VERSION_EXACT)
    include("${GameNetCore_DIR}/GameNetCoreConfigVersion.cmake")
    if(expected_compatible AND NOT PACKAGE_VERSION_COMPATIBLE)
        message(FATAL_ERROR "GameNetCore ${request} should be compatible with 0.3")
    endif()
    if(NOT expected_compatible AND PACKAGE_VERSION_COMPATIBLE)
        message(FATAL_ERROR "GameNetCore ${request} must be rejected by the 0.3 line")
    endif()
endfunction()

assert_gamenet_version(0.3.0 TRUE)
assert_gamenet_version(0.2.99 FALSE)
assert_gamenet_version(0.4.0 FALSE)
