add_library(gamenet_transport
    TcpTransportEndpoint.cc
)
add_library(GameNet::transport ALIAS gamenet_transport)

set_target_properties(gamenet_transport PROPERTIES EXPORT_NAME transport)

target_include_directories(gamenet_transport
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(gamenet_transport PUBLIC cxx_std_23)
target_compile_options(gamenet_transport PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_transport PUBLIC GameNet::core)
gamenet_configure_sanitizers(gamenet_transport)

install(TARGETS gamenet_transport
    EXPORT GameNetCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
