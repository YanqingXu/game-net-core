add_library(gamenet_game_session
    PlayerSession.cc
    SessionManager.cc
)
add_library(GameNet::game_session ALIAS gamenet_game_session)

set_target_properties(gamenet_game_session PROPERTIES EXPORT_NAME game_session)
target_include_directories(gamenet_game_session
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(gamenet_game_session PUBLIC cxx_std_23)
target_compile_options(gamenet_game_session PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_game_session PUBLIC GameNet::transport GameNet::core)
gamenet_configure_sanitizers(gamenet_game_session)

install(TARGETS gamenet_game_session
    EXPORT GameNetCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
