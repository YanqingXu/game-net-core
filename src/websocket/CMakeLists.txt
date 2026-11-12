# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
add_library(gamenet_websocket STATIC WebSocketCodec.cc WebSocketSession.cc)
add_library(GameNet::websocket ALIAS gamenet_websocket)
set_target_properties(gamenet_websocket PROPERTIES EXPORT_NAME websocket)
target_include_directories(gamenet_websocket PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(gamenet_websocket PUBLIC cxx_std_23)
target_compile_options(gamenet_websocket PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(gamenet_websocket PUBLIC GameNet::core)
gamenet_configure_sanitizers(gamenet_websocket)
install(TARGETS gamenet_websocket EXPORT GameNetWebSocketTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/gamenet/websocket DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gamenet)
install(EXPORT GameNetWebSocketTargets NAMESPACE GameNet:: DESTINATION ${GAMENET_INSTALL_CMAKEDIR})
