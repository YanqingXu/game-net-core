add_library(gamenet_core INTERFACE)
add_library(GameNet::core ALIAS gamenet_core)

target_include_directories(gamenet_core
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(gamenet_core INTERFACE cxx_std_23)

find_package(Threads REQUIRED)
target_link_libraries(gamenet_core INTERFACE Threads::Threads)

if(WIN32)
    target_compile_definitions(gamenet_core INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(gamenet_core INTERFACE ws2_32)
endif()

gamenet_configure_sanitizers(gamenet_core)
