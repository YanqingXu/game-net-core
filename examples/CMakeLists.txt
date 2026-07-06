add_executable(gamenet_echo_server
    echo_server/main.cpp
)

target_link_libraries(gamenet_echo_server PRIVATE GameNet::core)
set_target_properties(gamenet_echo_server PROPERTIES OUTPUT_NAME echo_server)

add_executable(gamenet_echo_client
    echo_client/main.cpp
)

target_link_libraries(gamenet_echo_client PRIVATE GameNet::core)
set_target_properties(gamenet_echo_client PROPERTIES OUTPUT_NAME echo_client)
