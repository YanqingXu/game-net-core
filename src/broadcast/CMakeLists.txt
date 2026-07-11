add_library(gamenet_broadcast
    BroadcastRouter.cc
    BroadcastDispatcher.cc
)
add_library(GameNet::broadcast ALIAS gamenet_broadcast)

set_target_properties(gamenet_broadcast PROPERTIES EXPORT_NAME broadcast)
target_include_directories(gamenet_broadcast
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(gamenet_broadcast PUBLIC cxx_std_23)
target_compile_options(gamenet_broadcast PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_broadcast PUBLIC GameNet::game_session GameNet::transport GameNet::core)
gamenet_configure_sanitizers(gamenet_broadcast)

install(TARGETS gamenet_broadcast
    EXPORT GameNetCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
