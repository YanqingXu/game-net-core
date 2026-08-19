set(GAMENET_CORE_SOURCES
    net/Acceptor.cc
    net/Buffer.cc
    base/Logger.cc
    metrics/MetricsExporter.cc
    metrics/MetricsHookRecorder.cc
    net/Channel.cc
    net/Connector.cc
    net/DeadlineQueue.cc
    net/detail/ConnectionBackpressureController.cc
    net/detail/PollerIoEngineAdapter.cc
    net/EventLoop.cc
    net/EventLoopThread.cc
    net/EventLoopThreadPool.cc
    net/InetAddress.cc
    net/NetworkMemoryRetention.cc
    net/Poller.cc
    net/Socket.cc
    net/TcpClient.cc
    net/TcpConnection.cc
    net/TcpOutputMemoryBudget.cc
    net/TcpServer.cc
    net/TimerQueue.cc
    net/poller/PollerFactory.cc
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND GAMENET_CORE_SOURCES
        net/platform/SocketsOps_win.cc
        net/platform/Wakeup_win.cc
        net/platform/IocpSocketOps_win.cc
        net/platform/IocpTcpTransport_win.cc
        net/poller/IocpPoller.cc
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND GAMENET_CORE_SOURCES
        net/detail/EpollReadinessPort.cc
        net/platform/SocketsOps_linux.cc
        net/platform/Wakeup_linux.cc
        net/poller/EPollPoller.cc
    )
else()
    message(FATAL_ERROR
        "GameNet::core has no backend for CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}'."
    )
endif()

add_library(gamenet_core STATIC ${GAMENET_CORE_SOURCES})
add_library(GameNet::core ALIAS gamenet_core)

set_target_properties(gamenet_core PROPERTIES EXPORT_NAME core)

target_include_directories(gamenet_core
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(gamenet_core PUBLIC cxx_std_23)
target_compile_options(gamenet_core
    PUBLIC
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/FS>
)

find_package(Threads REQUIRED)
target_link_libraries(gamenet_core PUBLIC Threads::Threads)

if(WIN32)
    target_compile_definitions(gamenet_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    if(GAMENET_BUILD_TESTING)
        target_compile_definitions(gamenet_core PRIVATE GAMENET_INTERNAL_IOCP_TEST_HOOKS=1)
    endif()
    target_link_libraries(gamenet_core PUBLIC ws2_32)
endif()

gamenet_configure_sanitizers(gamenet_core)

install(TARGETS gamenet_core
    EXPORT GameNetCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
