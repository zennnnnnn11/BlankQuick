add_library(service_config_model STATIC service_config_model.cpp)
target_include_directories(service_config_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(service_config_model PUBLIC Qt6::Core PRIVATE blank_shared)
blank_native_quality(service_config_model)
