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

find_package(Qt6 6.12 REQUIRED COMPONENTS Quick Network Qml Concurrent WebSockets)
if(NOT Qt6_VERSION VERSION_LESS "6.13")
    message(FATAL_ERROR "This project targets the Qt 6.12 release series.")
endif()
qt_standard_project_setup(REQUIRES 6.12)

include(cmake/TargetOptions.cmake)

add_library(blank_shared INTERFACE)
target_include_directories(blank_shared INTERFACE "${PROJECT_SOURCE_DIR}/src/shared/include")
target_include_directories(blank_shared SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/third_party")

add_library(rapidyaml STATIC third_party/rapidyaml/ryml.cpp)
set_target_properties(rapidyaml PROPERTIES AUTOMOC OFF AUTOUIC OFF CXX_CLANG_TIDY "")
target_include_directories(rapidyaml SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party")
target_compile_definitions(rapidyaml PUBLIC RYML_DEFAULT_CALLBACK_USES_EXCEPTIONS=1)
target_compile_options(rapidyaml PRIVATE /W0 /utf-8 /EHsc /GS /guard:cf /guard:ehcont)

function(blank_library target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${target} PRIVATE blank_shared)
    target_compile_definitions(${target} PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_BYTEARRAY QT_NO_URL_CAST_FROM_STRING
        QT_NO_CONTEXTLESS_CONNECT)
endfunction()

foreach(feature IN ITEMS nodes subscriptions profiles overrides runtime_config service_config)
    add_subdirectory(src/domain/${feature})
endforeach()
add_subdirectory(src/platform)
add_subdirectory(src/application/profiles)
add_subdirectory(src/application/runtime)
add_subdirectory(src/services/kernel)
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

qt_add_executable(blank_quick src/app/main.cpp cmake/application.manifest)
qt_add_qml_module(blank_quick URI Blank VERSION 1.0 QML_FILES qml/Main.qml)
target_link_libraries(blank_quick PRIVATE Qt6::Quick BlankQuick::Runtime dwmapi)
target_compile_options(blank_quick PRIVATE /guard:cf /guard:ehcont)
target_compile_definitions(blank_quick PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_BYTEARRAY QT_NO_URL_CAST_FROM_STRING
    QT_NO_NARROWING_CONVERSIONS_IN_CONNECT QT_NO_CONTEXTLESS_CONNECT QT_STRICT_ITERATORS
    QT_USE_QSTRINGBUILDER QT_DISABLE_DEPRECATED_UP_TO=0x060C00 QT_WARN_DEPRECATED_UP_TO=0x060C00)

include(cmake/SourceOptions.cmake)
include(cmake/Architecture.cmake)
blank_enforce_architecture()

target_link_options(blank_quick PRIVATE /WX /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf /guard:ehcont /CETCOMPAT /IGNORE:4291)
set_target_properties(blank_quick PROPERTIES WIN32_EXECUTABLE TRUE)

find_program(POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)
foreach(action IN ITEMS check format-check format clang-tidy clazy cppcheck qmllint)
    add_custom_target(${action}
        COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -File "${PROJECT_SOURCE_DIR}/tools/project.ps1"
            ${action} -Configuration "$<CONFIG>" -BuildDirectory "${PROJECT_BINARY_DIR}"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM
    )
endforeach()
add_dependencies(check blank_quick)
add_dependencies(check runtime_backend blank_quick_service system_proxy_guard kernel_signal subscription_fetcher)
if(BUILD_TESTING)
    add_dependencies(check test_binaries)
endif()

add_custom_target(quality ALL)
add_dependencies(quality check)

install(TARGETS blank_quick RUNTIME DESTINATION bin)
qt_generate_deploy_qml_app_script(TARGET blank_quick OUTPUT_SCRIPT deploy_script NO_TRANSLATIONS)
install(SCRIPT "${deploy_script}")
