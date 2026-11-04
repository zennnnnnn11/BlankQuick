find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Concurrent WebSockets)

# The migrated Windows libraries keep their original warning policy. The existing
# configuration/model libraries retain their stricter, independent policy.
function(backend_quality target)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus
        /Zc:preprocessor /sdl /guard:cf /guard:ehcont
        /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545
        /w14546 /w14547 /w14549 /w14555 /w14619 /w14826 /w14905 /w14906 /w14928)
    target_compile_definitions(${target} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
        PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII
        QT_NO_FOREACH QT_NO_CONTEXTLESS_CONNECT QT_ENABLE_STRICT_MODE_UP_TO=0x060A00
        QT_DISABLE_DEPRECATED_UP_TO=0x060A00 QT_WARN_DEPRECATED_UP_TO=0x060C00)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_link_options(${target} PRIVATE /WX /guard:cf /guard:ehcont /DYNAMICBASE
        /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES OFF)
endfunction()

function(backend_component name)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "foundation/${name}/*.cpp" "foundation/${name}/*.hpp")
    add_library(backend_${name} STATIC ${sources})
    target_include_directories(backend_${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/foundation/${name}/include")
    backend_quality(backend_${name})
endfunction()

add_library(backend_qt INTERFACE)
target_include_directories(backend_qt INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/foundation/qt/include")
target_link_libraries(backend_qt INTERFACE Qt6::Core)

backend_component(windows)
add_library(BlankQuick::Windows ALIAS backend_windows)
backend_component(storage)
target_link_libraries(backend_storage PUBLIC Qt6::Core PRIVATE backend_windows)
backend_component(config)
target_link_libraries(backend_config PUBLIC Qt6::Core)
target_include_directories(backend_config PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_include_directories(backend_config SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
backend_component(process)
target_link_libraries(backend_process PUBLIC Qt6::Core PRIVATE backend_windows Qt6::Network iphlpapi ws2_32)
backend_component(environment)
target_link_libraries(backend_environment PUBLIC Qt6::Core PRIVATE Qt6::Concurrent Qt6::Network iphlpapi ws2_32 user32)
backend_component(protocol)
target_link_libraries(backend_protocol PUBLIC Qt6::Core)
backend_component(ipc)
target_link_libraries(backend_ipc PUBLIC Qt6::Network backend_windows backend_protocol PRIVATE backend_qt advapi32 wtsapi32 shell32 ole32)
backend_component(tun)
target_link_libraries(backend_tun PUBLIC Qt6::Core PRIVATE backend_windows backend_storage Qt6::Network setupapi iphlpapi ws2_32 ole32)
backend_component(streams)
target_link_libraries(backend_streams PUBLIC Qt6::Network Qt6::WebSockets PRIVATE backend_qt)
backend_component(user_proxy)
target_link_libraries(backend_user_proxy PUBLIC Qt6::Core PRIVATE backend_windows wininet)
backend_component(runtime)
target_link_libraries(backend_runtime PUBLIC Qt6::Network backend_process PRIVATE backend_ipc backend_protocol Qt6::Concurrent)

add_library(backend_service STATIC service/service_host.cpp service/service_host.hpp)
target_include_directories(backend_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/service")
target_link_libraries(backend_service PUBLIC backend_ipc backend_process backend_config
    backend_environment backend_tun backend_streams Qt6::Concurrent PRIVATE advapi32)
backend_quality(backend_service)

add_executable(kernel_signal tools/kernel_signal.cpp)
target_link_libraries(kernel_signal PRIVATE backend_windows)
backend_quality(kernel_signal)
add_executable(system_proxy_guard tools/system_proxy_guard.cpp)
target_link_libraries(system_proxy_guard PRIVATE backend_user_proxy backend_process backend_windows)
backend_quality(system_proxy_guard)
add_executable(blank_quick_service service/service_main.cpp)
target_link_libraries(blank_quick_service PRIVATE backend_service backend_qt)
backend_quality(blank_quick_service)

add_library(runtime_backend STATIC session/runtime_session.cpp session/runtime_session.hpp
    session/runtime_contract.cpp session/runtime_contract.hpp
    session/config_switcher.cpp session/config_switcher.hpp
    session/system_proxy_controller.cpp session/system_proxy_controller.hpp
    session/traffic_controller.cpp session/traffic_controller.hpp)
target_include_directories(runtime_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/session")
target_link_libraries(runtime_backend PUBLIC runtime_config backend_runtime backend_environment
    backend_streams PRIVATE backend_config backend_user_proxy backend_ipc backend_protocol backend_qt Qt6::Concurrent)
backend_quality(runtime_backend)

set(backend_executables kernel_signal system_proxy_guard blank_quick_service)
foreach(target IN LISTS backend_executables)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/backend-bin")
endforeach()
