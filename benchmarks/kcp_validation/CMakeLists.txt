# Copyright 2026 Yanqing Xu
# SPDX-License-Identifier: Apache-2.0
# Separate default-OFF C4 tool, outside installed libraries and HP0 scenarios.
add_executable(gamenet_kcp_validation main.cpp)
target_link_libraries(gamenet_kcp_validation PRIVATE GameNet::kcp)
target_compile_features(gamenet_kcp_validation PRIVATE cxx_std_23)
target_compile_options(gamenet_kcp_validation PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
if(WIN32)
    target_link_libraries(gamenet_kcp_validation PRIVATE psapi)
endif()
gamenet_configure_sanitizers(gamenet_kcp_validation)
