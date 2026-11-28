cmake_minimum_required(VERSION 3.24)
project(EZ_GPT_SoVITS VERSION 0.1.23 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "EZ-GPT-SoVITS requires Windows x64")
endif()
enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INSTALL_BINDIR ".")
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network Sql Multimedia Concurrent)
qt_add_executable(EZ-GPT-SoVITS WIN32
    assets/app-icon.rc
    src/main.cpp
    src/ui.cpp src/ui.h
    src/engine.cpp src/engine.h
    src/compatibility.cpp src/compatibility.h
    src/resources.cpp src/resources.h
    src/diagnostics.cpp src/diagnostics.h
    src/jobs.cpp src/jobs.h
    src/settings.cpp src/settings.h
    src/workspacepaths.cpp src/workspacepaths.h
    src/library.cpp src/library.h
    src/synthesis.cpp src/synthesis.h
    src/voiceverification.cpp src/voiceverification.h
    src/training.cpp src/training.h
    src/recordings.cpp src/recordings.h
    src/scriptlines.cpp src/scriptlines.h
    src/waveexport.cpp src/waveexport.h
)
target_include_directories(EZ-GPT-SoVITS PRIVATE assets)
target_compile_definitions(EZ-GPT-SoVITS PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN EZ_APP_VERSION="${PROJECT_VERSION}")
target_compile_options(EZ-GPT-SoVITS PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(EZ-GPT-SoVITS PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Multimedia Qt6::Concurrent iphlpapi ws2_32 dwmapi
)
set(EZ_ENGINE_CONTRACTS engine/20261006-contract.json engine/0725-cu124-contract.json)
qt_add_resources(EZ-GPT-SoVITS "engine_bridge"
    PREFIX "/"
    FILES engine/bridge.py ${EZ_ENGINE_CONTRACTS} compat/registry-v1.json
)
qt_add_resources(EZ-GPT-SoVITS "branding"
    PREFIX "/branding"
    BASE assets
    FILES assets/app-icon.png
)
qt_add_resources(EZ-GPT-SoVITS "control_icons"
    PREFIX "/controls"
    BASE assets
    FILES assets/chevron-down.png assets/chevron-up.png assets/checkmark.png
)
qt_add_executable(EZ-Compatibility-Wizard WIN32
    assets/app-icon.rc
    src/compatwizard.cpp src/compatdraft.cpp src/compatdraft.h
    src/engine.cpp src/engine.h src/compatibility.cpp src/compatibility.h
    src/resources.cpp src/resources.h src/ui.cpp src/ui.h
)
target_include_directories(EZ-Compatibility-Wizard PRIVATE assets)
target_compile_definitions(EZ-Compatibility-Wizard PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN EZ_APP_VERSION="${PROJECT_VERSION}")
target_compile_options(EZ-Compatibility-Wizard PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(EZ-Compatibility-Wizard PRIVATE Qt6::Widgets Qt6::Network Qt6::Concurrent iphlpapi ws2_32)
qt_add_resources(EZ-Compatibility-Wizard "wizard_registry"
    PREFIX "/"
    FILES ${EZ_ENGINE_CONTRACTS} compat/registry-v1.json
)
qt_add_resources(EZ-Compatibility-Wizard "wizard_branding"
    PREFIX "/branding"
    BASE assets
    FILES assets/app-icon.png
)
qt_add_resources(EZ-Compatibility-Wizard "wizard_controls"
    PREFIX "/controls"
    BASE assets
    FILES assets/chevron-down.png assets/chevron-up.png assets/checkmark.png
)
install(TARGETS EZ-GPT-SoVITS EZ-Compatibility-Wizard RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES README.md THIRD_PARTY.md DESTINATION .)
install(FILES ${EZ_ENGINE_CONTRACTS} DESTINATION engine)
install(FILES compat/registry-v1.json compat/acceptance-20261006.md
              compat/acceptance-0725-cu124.md compat/registration-template.md
              compat/adaptation-checklist.md compat/quickstart-zh.md DESTINATION compat)
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

option(EZ_BUILD_CHECKS "Build the optional Windows component checks" OFF)
if(EZ_BUILD_CHECKS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    qt_add_executable(EZ-component-checks
        tests/workflow-components.cpp
        src/waveexport.cpp src/recordings.cpp src/engine.cpp src/resources.cpp src/compatibility.cpp
        src/scriptlines.cpp src/compatdraft.cpp src/synthesis.cpp
        src/training.cpp src/jobs.cpp src/settings.cpp src/workspacepaths.cpp src/voiceverification.cpp src/ui.cpp
    )
    target_include_directories(EZ-component-checks PRIVATE src)
    target_compile_definitions(EZ-component-checks PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN EZ_APP_VERSION="${PROJECT_VERSION}")
    qt_add_resources(EZ-component-checks "test_engine_bridge"
        PREFIX "/" FILES engine/bridge.py ${EZ_ENGINE_CONTRACTS} compat/registry-v1.json)
    target_compile_options(EZ-component-checks PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(EZ-component-checks PRIVATE Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::Concurrent Qt6::Sql Qt6::Test iphlpapi ws2_32)
endif()
