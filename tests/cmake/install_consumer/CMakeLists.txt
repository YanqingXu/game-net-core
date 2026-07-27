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
        GameNet::protocol
        GameNet::transport
        GameNet::game_session
        GameNet::game_logic
        GameNet::broadcast
)

enable_testing()
add_test(NAME gamenet.install_consumer COMMAND gamenet_install_consumer)
