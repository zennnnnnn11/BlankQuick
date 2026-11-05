add_library(runtime_backend STATIC runtime_session.cpp runtime_contract.cpp config_switcher.cpp
    configuration_preparation.cpp
    include/blank/runtime/runtime_session.hpp include/blank/runtime/runtime_contract.hpp
    include/blank/runtime/config_switcher.hpp)
target_include_directories(runtime_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(runtime_backend PUBLIC runtime_config profile_runtime overrides backend_runtime
    backend_environment system_proxy telemetry override_storage runtime_settings_storage runtime_kernel_check backend_streams
    PRIVATE runtime_context override_script backend_config backend_ipc backend_protocol backend_qt Qt6::Concurrent)
blank_native_quality(runtime_backend)
add_library(BlankQuick::Runtime ALIAS runtime_backend)
