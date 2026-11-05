blank_library(runtime_config mihomo_export.cpp mihomo_export_fields.inc runtime_config.cpp
    runtime_settings.cpp runtime_validation.cpp runtime_schema.inc settings_codec.cpp)
target_link_libraries(runtime_config PUBLIC profile_payload overrides blank_shared PRIVATE Qt6::Core)
add_library(BlankQuick::RuntimeConfig ALIAS runtime_config)
