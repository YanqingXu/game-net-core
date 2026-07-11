add_library(gamenet_protocol
    PacketFramer.cc
)
add_library(GameNet::protocol ALIAS gamenet_protocol)

set_target_properties(gamenet_protocol PROPERTIES EXPORT_NAME protocol)

target_include_directories(gamenet_protocol
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(gamenet_protocol PUBLIC cxx_std_23)
target_compile_options(gamenet_protocol
    PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

gamenet_configure_sanitizers(gamenet_protocol)

install(TARGETS gamenet_protocol
    EXPORT GameNetCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
