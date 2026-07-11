cmake_minimum_required(VERSION 3.20)

project(GameNetInstallConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(GameNetCore REQUIRED)

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
