# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
add_library(gamenet_websocket_tls STATIC WebSocketTlsSession.cc)
add_library(GameNet::websocket_tls ALIAS gamenet_websocket_tls)
set_target_properties(gamenet_websocket_tls PROPERTIES EXPORT_NAME websocket_tls)
target_include_directories(gamenet_websocket_tls PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(gamenet_websocket_tls PUBLIC cxx_std_23)
target_compile_options(gamenet_websocket_tls PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_websocket_tls PUBLIC GameNet::websocket GameNet::tls)
gamenet_configure_sanitizers(gamenet_websocket_tls)
install(TARGETS gamenet_websocket_tls EXPORT GameNetWebSocketTlsTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/websocket_tls DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet)
install(EXPORT GameNetWebSocketTlsTargets NAMESPACE GameNet:: DESTINATION ${GAMENET_INSTALL_CMAKEDIR})
