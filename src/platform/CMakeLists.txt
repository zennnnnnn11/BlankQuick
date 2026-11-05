find_package(Qt6 6.12 REQUIRED COMPONENTS Core Network Concurrent WebSockets)
add_library(backend_qt INTERFACE)
target_include_directories(backend_qt INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/qt/include")
target_link_libraries(backend_qt INTERFACE Qt6::Core)

add_library(backend_windows STATIC windows/handle.cpp windows/include/blank/windows/handle.hpp)
target_include_directories(backend_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/windows/include")
blank_native_quality(backend_windows)

add_library(backend_storage STATIC storage/file_durability.cpp storage/include/blank/storage/file_durability.hpp)
target_include_directories(backend_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/storage/include")
target_link_libraries(backend_storage PUBLIC Qt6::Core PRIVATE backend_windows)
blank_native_quality(backend_storage)

add_library(backend_config STATIC config/service_config.cpp config/include/blank/config/service_config.hpp)
target_include_directories(backend_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/config/include")
target_link_libraries(backend_config PUBLIC service_config_model PRIVATE Qt6::Core)
blank_native_quality(backend_config)

add_library(backend_process STATIC process/native_kernel.cpp process/kernel_workspace.cpp
    process/kernel_workspace.hpp process/include/blank/process/native_kernel.hpp)
target_include_directories(backend_process PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/process/include")
target_link_libraries(backend_process PUBLIC Qt6::Core PRIVATE backend_windows Qt6::Network iphlpapi ws2_32)
blank_native_quality(backend_process)

add_library(backend_environment STATIC environment/network_inventory.cpp environment/environment_monitor.cpp environment/include/blank/environment/environment_monitor.hpp environment/include/blank/environment/network_inventory.hpp)
target_include_directories(backend_environment PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/environment/include")
target_link_libraries(backend_environment PUBLIC Qt6::Core PRIVATE backend_qt Qt6::Concurrent Qt6::Network iphlpapi ws2_32 user32)
blank_native_quality(backend_environment)

add_library(backend_protocol STATIC protocol/service_api.cpp protocol/json_frame.cpp protocol/include/blank/protocol/json_frame.hpp protocol/include/blank/protocol/service_api.hpp)
target_include_directories(backend_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/protocol/include")
target_link_libraries(backend_protocol PUBLIC Qt6::Core blank_shared)
blank_native_quality(backend_protocol)

add_library(backend_ipc STATIC ipc/service_setup.cpp ipc/service_security.cpp ipc/service_pipe.cpp ipc/service_client.cpp ipc/include/blank/ipc/service_client.hpp ipc/include/blank/ipc/service_pipe.hpp ipc/include/blank/ipc/service_security.hpp ipc/include/blank/ipc/service_setup.hpp)
target_include_directories(backend_ipc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ipc/include")
target_link_libraries(backend_ipc PUBLIC Qt6::Network backend_windows backend_protocol PRIVATE backend_qt advapi32 wtsapi32 shell32 ole32)
blank_native_quality(backend_ipc)

add_library(backend_tun STATIC tun/tun_resources.cpp tun/include/blank/tun/tun_resources.hpp)
target_include_directories(backend_tun PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tun/include")
target_link_libraries(backend_tun PUBLIC Qt6::Core PRIVATE backend_windows backend_storage Qt6::Network setupapi iphlpapi ws2_32 ole32)
blank_native_quality(backend_tun)

add_library(BlankQuick::Windows ALIAS backend_windows)
add_executable(kernel_signal process/kernel_signal.cpp)
target_link_libraries(kernel_signal PRIVATE backend_windows)
blank_native_quality(kernel_signal)
set_target_properties(kernel_signal PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/backend-bin")

add_library(backend_runtime STATIC kernel/kernel_controller.cpp
    kernel/include/blank/runtime/kernel_controller.hpp)
target_include_directories(backend_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/kernel/include")
target_link_libraries(backend_runtime PUBLIC Qt6::Network backend_process PRIVATE backend_ipc backend_protocol backend_qt Qt6::Concurrent)
blank_native_quality(backend_runtime)

# Feature-specific effects depend inward on data, never on the application.
foreach(adapter IN ITEMS subscriptions profiles overrides scripting configuration system_proxy telemetry)
    add_subdirectory(${adapter})
endforeach()

