add_library(backend_service STATIC service_host.cpp service_host.hpp)
target_include_directories(backend_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(backend_service PUBLIC backend_ipc backend_process backend_config
    backend_environment backend_tun backend_streams Qt6::Concurrent PRIVATE backend_qt advapi32)
blank_native_quality(backend_service)
add_executable(blank_quick_service service_main.cpp)
target_link_libraries(blank_quick_service PRIVATE backend_service backend_qt)
blank_native_quality(blank_quick_service)
set_target_properties(blank_quick_service PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/backend-bin")
