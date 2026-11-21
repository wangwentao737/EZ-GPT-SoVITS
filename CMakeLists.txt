cmake_minimum_required(VERSION 3.24)
project(EZ_GPT_SoVITS VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EZ-GPT-SoVITS requires Windows x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INSTALL_BINDIR ".")
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network Sql Multimedia Concurrent)
qt_add_executable(EZ-GPT-SoVITS WIN32
    src/main.cpp
    src/engine.cpp src/engine.h
    src/jobs.cpp src/jobs.h
    src/synthesis.cpp src/synthesis.h
    src/training.cpp src/training.h
)
target_compile_definitions(EZ-GPT-SoVITS PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(EZ-GPT-SoVITS PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(EZ-GPT-SoVITS PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Multimedia Qt6::Concurrent iphlpapi ws2_32
)
qt_add_resources(EZ-GPT-SoVITS "engine_bridge"
    PREFIX "/"
    FILES engine/bridge.py
)
install(TARGETS EZ-GPT-SoVITS RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES README.md THIRD_PARTY.md 开发文档.md DESTINATION .)
get_filename_component(qt_sdk_root "${Qt6_DIR}/../../.." ABSOLUTE)
if(IS_DIRECTORY "${qt_sdk_root}/sbom")
    install(DIRECTORY "${qt_sdk_root}/sbom/" DESTINATION licenses/qt-sbom
        FILES_MATCHING
        PATTERN "qtbase-*.spdx"
        PATTERN "qtmultimedia-*.spdx"
        PATTERN "qtsvg-*.spdx"
        PATTERN "qttranslations-*.spdx"
    )
endif()
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
include(InstallRequiredSystemLibraries)
qt_generate_deploy_app_script(TARGET EZ-GPT-SoVITS OUTPUT_SCRIPT deploy_script NO_COMPILER_RUNTIME)
install(SCRIPT ${deploy_script})
