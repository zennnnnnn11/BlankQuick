cmake_minimum_required(VERSION 3.30)

project(BlankQuick VERSION 0.1.0 LANGUAGES CXX)
cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2 CMAKEFILES 1)

if(EXISTS "${PROJECT_SOURCE_DIR}/out/toolchain/cl-includes-prefix.txt")
    file(READ "${PROJECT_SOURCE_DIR}/out/toolchain/cl-includes-prefix.txt" CMAKE_CL_SHOWINCLUDES_PREFIX)
endif()

if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "Use native MSVC x64 with tools/project.ps1.")
endif()
if(MSVC_VERSION LESS 1944)
    message(FATAL_ERROR "MSVC 19.44 or newer is required.")
endif()

add_compile_options(/EHsc)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Quick Network Qml)
if(NOT Qt6_VERSION VERSION_LESS "6.13")
    message(FATAL_ERROR "This project targets the Qt 6.12 release series.")
endif()
qt_standard_project_setup(REQUIRES 6.12)

file(GLOB_RECURSE project_sources CONFIGURE_DEPENDS "src/*.cpp" "src/*.h" "src/*.hpp")
list(REMOVE_ITEM project_sources "${PROJECT_SOURCE_DIR}/src/node_input.cpp"
    "${PROJECT_SOURCE_DIR}/src/subscription_fetcher.cpp" "${PROJECT_SOURCE_DIR}/src/subscription_metadata.cpp"
    "${PROJECT_SOURCE_DIR}/src/profile_storage.cpp" "${PROJECT_SOURCE_DIR}/src/profile_codec.cpp"
    "${PROJECT_SOURCE_DIR}/src/profile_digest.cpp"
    "${PROJECT_SOURCE_DIR}/src/node_filter.cpp" "${PROJECT_SOURCE_DIR}/src/minimal_profile.cpp"
    "${PROJECT_SOURCE_DIR}/src/profile_runtime.cpp"
    "${PROJECT_SOURCE_DIR}/src/override_document.cpp" "${PROJECT_SOURCE_DIR}/src/override_engine.cpp"
    "${PROJECT_SOURCE_DIR}/src/override_script.cpp" "${PROJECT_SOURCE_DIR}/src/override_manager.cpp")
list(REMOVE_ITEM project_sources "${PROJECT_SOURCE_DIR}/src/mihomo_export.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime_config.cpp" "${PROJECT_SOURCE_DIR}/src/runtime_settings.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime_validation.cpp" "${PROJECT_SOURCE_DIR}/src/runtime_settings_storage.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime_kernel_check.cpp")
add_library(node_filter STATIC src/node_filter.cpp)
target_include_directories(node_filter PUBLIC "${PROJECT_SOURCE_DIR}/src")
add_library(rapidyaml STATIC third_party/rapidyaml/ryml.cpp)
set_target_properties(rapidyaml PROPERTIES AUTOMOC OFF AUTOUIC OFF CXX_CLANG_TIDY "")
target_include_directories(rapidyaml SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party")
target_compile_definitions(rapidyaml PUBLIC RYML_DEFAULT_CALLBACK_USES_EXCEPTIONS=1)
target_compile_options(rapidyaml PRIVATE /W0 /utf-8 /EHsc /GS /guard:cf /guard:ehcont)
add_library(node_input STATIC src/node_input.cpp src/node_input_bindings.inc
    src/node_input_document.inc src/node_input_yaml.inc src/node_input_singbox.inc src/node_input_wireguard.inc)
target_include_directories(node_input PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(node_input SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(node_input PRIVATE rapidyaml)
add_library(profile_payload STATIC src/minimal_profile.cpp)
target_include_directories(profile_payload PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(profile_payload SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(profile_payload PRIVATE node_input node_filter)
add_library(subscription_fetcher STATIC src/subscription_fetcher.cpp src/subscription_metadata.cpp)
target_compile_options(subscription_fetcher PRIVATE /EHsc)
target_include_directories(subscription_fetcher PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(subscription_fetcher PUBLIC node_input PRIVATE Qt6::Network)
target_compile_definitions(subscription_fetcher PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_BYTEARRAY QT_NO_URL_CAST_FROM_STRING QT_NO_CONTEXTLESS_CONNECT)
add_library(profile_storage STATIC src/profile_storage.cpp src/profile_digest.cpp src/profile_codec.cpp src/profile_codec_fields.inc)
target_include_directories(profile_storage PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(profile_storage SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(profile_storage PRIVATE Qt6::Core bcrypt)
set_property(SOURCE src/profile_digest.cpp APPEND PROPERTY COMPILE_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(profile_storage PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_BYTEARRAY QT_NO_URL_CAST_FROM_STRING QT_NO_CONTEXTLESS_CONNECT)
add_library(profile_runtime STATIC src/profile_runtime.cpp)
target_include_directories(profile_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(profile_runtime PUBLIC profile_storage profile_payload)
add_library(overrides STATIC src/override_document.cpp src/override_engine.cpp
    src/override_script.cpp src/override_manager.cpp)
target_include_directories(overrides PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(overrides SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(overrides PRIVATE rapidyaml Qt6::Qml)
target_compile_definitions(overrides PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_BYTEARRAY QT_NO_URL_CAST_FROM_STRING QT_NO_CONTEXTLESS_CONNECT)
add_library(runtime_config STATIC src/mihomo_export.cpp src/mihomo_export_fields.inc
    src/runtime_config.cpp src/runtime_settings.cpp src/runtime_validation.cpp src/runtime_schema.inc
    src/runtime_settings_storage.cpp src/runtime_kernel_check.cpp)
target_include_directories(runtime_config PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_include_directories(runtime_config SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_link_libraries(runtime_config PUBLIC profile_payload overrides PRIVATE Qt6::Core Qt6::Network)
target_compile_definitions(runtime_config PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_BYTEARRAY QT_NO_URL_CAST_FROM_STRING QT_NO_CONTEXTLESS_CONNECT)
qt_add_executable(blank_quick ${project_sources} cmake/application.manifest)
qt_add_qml_module(blank_quick
    URI Blank
    VERSION 1.0
    QML_FILES qml/Main.qml
)
target_link_libraries(blank_quick PRIVATE Qt6::Quick node_input subscription_fetcher profile_storage node_filter profile_payload profile_runtime overrides runtime_config)
target_compile_options(blank_quick PRIVATE /guard:cf /guard:ehcont)
target_compile_definitions(blank_quick PRIVATE
    QT_NO_KEYWORDS
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_CAST_FROM_BYTEARRAY
    QT_NO_URL_CAST_FROM_STRING
    QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
    QT_NO_CONTEXTLESS_CONNECT
    QT_STRICT_ITERATORS
    QT_USE_QSTRINGBUILDER
    QT_DISABLE_DEPRECATED_UP_TO=0x060C00
    QT_WARN_DEPRECATED_UP_TO=0x060C00
)

include(cmake/StrictMSVC.cmake)
set_source_files_properties(${project_sources} PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_source_files_properties(src/subscription_fetcher.cpp src/subscription_metadata.cpp src/node_input.cpp src/profile_storage.cpp src/profile_digest.cpp src/profile_codec.cpp src/node_filter.cpp src/minimal_profile.cpp src/profile_runtime.cpp PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_source_files_properties(src/override_document.cpp src/override_engine.cpp src/override_script.cpp
    src/override_manager.cpp PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_source_files_properties(src/mihomo_export.cpp src/runtime_config.cpp src/runtime_settings.cpp
    src/runtime_validation.cpp src/runtime_settings_storage.cpp src/runtime_kernel_check.cpp
    PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_property(SOURCE src/mihomo_export.cpp src/runtime_config.cpp src/runtime_settings.cpp
    src/runtime_validation.cpp src/runtime_settings_storage.cpp src/runtime_kernel_check.cpp
    APPEND PROPERTY COMPILE_OPTIONS /wd4820 /wd4625 /wd4626 /wd4514 /wd26472 /wd26821)
set_property(SOURCE src/runtime_settings_storage.cpp APPEND PROPERTY COMPILE_OPTIONS /wd5026 /wd5027)
# The new independent module uses the same ISO aggregate/span conventions above.
set_property(SOURCE src/override_document.cpp src/override_engine.cpp src/override_script.cpp
    src/override_manager.cpp APPEND PROPERTY COMPILE_OPTIONS /wd4820 /wd4625 /wd4626 /wd4514 /wd26472 /wd26821)
# nlohmann ordered_map preserves YAML operation order. MSVC diagnoses the vendor
# pair construction evaluation order even for independent operands.
set_property(SOURCE src/override_document.cpp src/override_manager.cpp APPEND PROPERTY COMPILE_OPTIONS /wd4868)
set_property(SOURCE src/mihomo_export.cpp src/runtime_config.cpp src/runtime_settings.cpp
    src/runtime_validation.cpp src/runtime_settings_storage.cpp APPEND PROPERTY COMPILE_OPTIONS /wd4866 /wd4868)
# QLockFile intentionally makes the private lock session nonmovable (Rule of Zero).
set_property(SOURCE src/override_manager.cpp APPEND PROPERTY COMPILE_OPTIONS /wd5026 /wd5027)
# Model aggregates use natural alignment and move-only ownership; these /Wall
# diagnostics concern padding, implicit deleted copies and unused inline helpers.
set_property(SOURCE src/unified_node.cpp src/node_input.cpp src/subscription_fetcher.cpp src/subscription_metadata.cpp src/profile_storage.cpp src/profile_codec.cpp src/node_filter.cpp src/minimal_profile.cpp src/profile_runtime.cpp APPEND PROPERTY COMPILE_OPTIONS
    /wd4820 /wd4625 /wd4626 /wd4514
)
# The ISO C++ codec and native digest adapter cast after validating octet/range bounds;
# C26472 requests GSL narrow/narrow_cast even for these checked conversions.
set_property(SOURCE src/node_input.cpp src/subscription_fetcher.cpp src/subscription_metadata.cpp src/profile_codec.cpp src/profile_digest.cpp src/node_filter.cpp src/minimal_profile.cpp APPEND PROPERTY COMPILE_OPTIONS /wd26472)
# ISO C++20 std::span is intentional; C26821 asks for a GSL dependency instead.
set_property(SOURCE src/subscription_metadata.cpp src/subscription_fetcher.cpp src/profile_storage.cpp src/profile_codec.cpp src/minimal_profile.cpp APPEND PROPERTY COMPILE_OPTIONS /wd26821)

# Qt 6.12 QMetaType instantiates QDebug << QNetworkProxy for the auth signal;
# MSVC C4866 diagnoses the vendor operator evaluation order, not our callback.
set_property(SOURCE src/subscription_fetcher.cpp APPEND PROPERTY COMPILE_OPTIONS /wd4866)
# Private reflection stores pointers-to-members, never persisted as binary.
# C4371 only reports a layout difference from historic MSVC compiler ABIs.
set_property(SOURCE src/profile_codec.cpp APPEND PROPERTY COMPILE_OPTIONS /wd4371)
# State deliberately owns a nonmovable mutex; sessions share it through shared_ptr.
# Keep Rule of Zero rather than spelling five artificial special members.
set_property(SOURCE src/profile_runtime.cpp src/profile_storage.cpp APPEND PROPERTY COMPILE_OPTIONS /wd5026 /wd5027)

target_link_options(blank_quick PRIVATE /WX /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf /guard:ehcont /CETCOMPAT /IGNORE:4291)
set_target_properties(blank_quick PROPERTIES WIN32_EXECUTABLE TRUE)

find_program(POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)
foreach(action IN ITEMS check format-check format clang-tidy clazy cppcheck qmllint)
    add_custom_target(${action}
        COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -File "${PROJECT_SOURCE_DIR}/tools/project.ps1"
            ${action} -Configuration "$<CONFIG>"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM
    )
endforeach()
add_dependencies(check blank_quick)

add_subdirectory(backend)

add_dependencies(check runtime_backend blank_quick_service system_proxy_guard kernel_signal)

add_custom_target(quality ALL)
add_dependencies(quality check)

install(TARGETS blank_quick RUNTIME DESTINATION bin)
qt_generate_deploy_qml_app_script(TARGET blank_quick OUTPUT_SCRIPT deploy_script NO_TRANSLATIONS)
install(SCRIPT "${deploy_script}")
