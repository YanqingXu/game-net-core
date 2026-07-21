add_library(gamenet_game_logic
    GameCommandQueue.cc
    LogicMetricsRecorder.cc
    LogicLoop.cc
)
add_library(GameNet::game_logic ALIAS gamenet_game_logic)

set_target_properties(gamenet_game_logic PROPERTIES EXPORT_NAME game_logic)
target_include_directories(gamenet_game_logic
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(gamenet_game_logic PUBLIC cxx_std_23)
target_compile_options(gamenet_game_logic PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_game_logic PUBLIC GameNet::game_session GameNet::core)
if(GAMENET_BUILD_TESTING)
    target_compile_definitions(gamenet_game_logic PRIVATE GAMENET_ENABLE_INTERNAL_TEST_HOOKS=1)
endif()
gamenet_configure_sanitizers(gamenet_game_logic)

install(TARGETS gamenet_game_logic
    EXPORT GameNetCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
