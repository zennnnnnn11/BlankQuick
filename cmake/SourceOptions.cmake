include("${CMAKE_CURRENT_LIST_DIR}/StrictMSVC.cmake")
set(feature_directories
    "${PROJECT_SOURCE_DIR}/src/platform/profiles"
    "${PROJECT_SOURCE_DIR}/src/platform/overrides"
    "${PROJECT_SOURCE_DIR}/src/platform/scripting"
    "${PROJECT_SOURCE_DIR}/src/platform/subscriptions"
    "${PROJECT_SOURCE_DIR}/src/platform/configuration"
    "${PROJECT_SOURCE_DIR}/src/application/profiles"
    "${PROJECT_SOURCE_DIR}/src/domain/nodes"
    "${PROJECT_SOURCE_DIR}/src/domain/subscriptions"
    "${PROJECT_SOURCE_DIR}/src/domain/profiles"
    "${PROJECT_SOURCE_DIR}/src/domain/overrides"
    "${PROJECT_SOURCE_DIR}/src/domain/runtime_config"
)

set_source_files_properties(
    src/app/main.cpp
    PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
# Qt's ColorScheme signal instantiates a vendor QMetaType debug operator with C4866.
set_property(SOURCE src/app/main.cpp APPEND PROPERTY COMPILE_OPTIONS /wd4866)
set_source_files_properties(
    src/domain/nodes/unified_node.cpp
    src/platform/subscriptions/subscription_fetcher.cpp
    src/domain/subscriptions/subscription_metadata.cpp
    src/domain/nodes/node_input.cpp
    src/platform/profiles/profile_storage.cpp
    src/platform/profiles/profile_digest.cpp
    src/domain/profiles/profile_codec.cpp
    src/domain/profiles/node_filter.cpp
    src/domain/profiles/minimal_profile.cpp
    src/application/profiles/profile_runtime.cpp
    DIRECTORY ${feature_directories}
    PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_source_files_properties(src/domain/overrides/override_document.cpp src/domain/overrides/override_engine.cpp src/platform/scripting/override_script.cpp
    src/platform/overrides/override_manager.cpp DIRECTORY ${feature_directories} PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_source_files_properties(src/domain/runtime_config/mihomo_export.cpp src/domain/runtime_config/runtime_config.cpp src/domain/runtime_config/runtime_settings.cpp
    src/domain/runtime_config/runtime_validation.cpp src/platform/configuration/runtime_settings_storage.cpp src/platform/configuration/runtime_kernel_check.cpp
    DIRECTORY ${feature_directories} PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_property(SOURCE src/domain/runtime_config/mihomo_export.cpp src/domain/runtime_config/runtime_config.cpp src/domain/runtime_config/runtime_settings.cpp
    src/domain/runtime_config/runtime_validation.cpp src/platform/configuration/runtime_settings_storage.cpp src/platform/configuration/runtime_kernel_check.cpp
    DIRECTORY ${feature_directories} APPEND PROPERTY COMPILE_OPTIONS /wd4820 /wd4625 /wd4626 /wd4514 /wd26472 /wd26821)
set_property(SOURCE
    src/platform/configuration/runtime_settings_storage.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd5026 /wd5027)
# The new independent module uses the same ISO aggregate/span conventions above.
set_property(SOURCE src/domain/overrides/override_document.cpp src/domain/overrides/override_engine.cpp src/platform/scripting/override_script.cpp
    src/platform/overrides/override_manager.cpp DIRECTORY ${feature_directories} APPEND PROPERTY COMPILE_OPTIONS /wd4820 /wd4625 /wd4626 /wd4514 /wd26472 /wd26821)
# nlohmann ordered_map preserves YAML operation order. MSVC diagnoses the vendor
# pair construction evaluation order even for independent operands.
set_property(SOURCE
    src/domain/overrides/override_document.cpp
    src/platform/overrides/override_manager.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd4868)
set_property(SOURCE src/domain/runtime_config/mihomo_export.cpp src/domain/runtime_config/runtime_config.cpp src/domain/runtime_config/runtime_settings.cpp
    src/domain/runtime_config/runtime_validation.cpp src/platform/configuration/runtime_settings_storage.cpp DIRECTORY ${feature_directories} APPEND PROPERTY COMPILE_OPTIONS /wd4866 /wd4868)
# QLockFile intentionally makes the private lock session nonmovable (Rule of Zero).
set_property(SOURCE
    src/platform/overrides/override_manager.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd5026 /wd5027)
# Model aggregates use natural alignment and move-only ownership; these /Wall
# diagnostics concern padding, implicit deleted copies and unused inline helpers.
set_property(SOURCE src/domain/nodes/unified_node.cpp src/domain/nodes/node_input.cpp src/platform/subscriptions/subscription_fetcher.cpp src/domain/subscriptions/subscription_metadata.cpp src/platform/profiles/profile_storage.cpp src/domain/profiles/profile_codec.cpp src/domain/profiles/node_filter.cpp src/domain/profiles/minimal_profile.cpp src/application/profiles/profile_runtime.cpp DIRECTORY ${feature_directories} APPEND PROPERTY COMPILE_OPTIONS
    /wd4820 /wd4625 /wd4626 /wd4514
)
# The ISO C++ codec and native digest adapter cast after validating octet/range bounds;
# C26472 requests GSL narrow/narrow_cast even for these checked conversions.
set_property(SOURCE
    src/domain/nodes/node_input.cpp
    src/platform/subscriptions/subscription_fetcher.cpp
    src/domain/subscriptions/subscription_metadata.cpp
    src/domain/profiles/profile_codec.cpp
    src/platform/profiles/profile_digest.cpp
    src/domain/profiles/node_filter.cpp
    src/domain/profiles/minimal_profile.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd26472)
# ISO C++20 std::span is intentional; C26821 asks for a GSL dependency instead.
set_property(SOURCE
    src/domain/subscriptions/subscription_metadata.cpp
    src/platform/subscriptions/subscription_fetcher.cpp
    src/platform/profiles/profile_storage.cpp
    src/domain/profiles/profile_codec.cpp
    src/domain/profiles/minimal_profile.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd26821)

# Qt 6.12 QMetaType instantiates QDebug << QNetworkProxy for the auth signal;
# MSVC C4866 diagnoses the vendor operator evaluation order, not our callback.
set_property(SOURCE
    src/platform/subscriptions/subscription_fetcher.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd4866)
# Private reflection stores pointers-to-members, never persisted as binary.
# C4371 only reports a layout difference from historic MSVC compiler ABIs.
set_property(SOURCE
    src/domain/profiles/profile_codec.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd4371)
# State deliberately owns a nonmovable mutex; sessions share it through shared_ptr.
# Keep Rule of Zero rather than spelling five artificial special members.
set_property(SOURCE
    src/application/profiles/profile_runtime.cpp
    src/platform/profiles/profile_storage.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd5026 /wd5027)


set_source_files_properties(src/domain/runtime_config/settings_codec.cpp
    src/platform/configuration/runtime_context.cpp src/domain/profiles/profile_types.cpp
    DIRECTORY ${feature_directories}
    PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_property(SOURCE src/domain/runtime_config/settings_codec.cpp
    src/platform/configuration/runtime_context.cpp src/domain/profiles/profile_types.cpp
    DIRECTORY ${feature_directories}
    APPEND PROPERTY COMPILE_OPTIONS /wd4820 /wd4625 /wd4626 /wd4514 /wd26472 /wd26821 /wd4868)

set_source_files_properties(src/domain/subscriptions/subscription_response.cpp
    DIRECTORY ${feature_directories} PROPERTIES COMPILE_OPTIONS "${strict_msvc_options}")
set_property(SOURCE src/domain/subscriptions/subscription_response.cpp
    DIRECTORY ${feature_directories} APPEND PROPERTY COMPILE_OPTIONS /wd4820 /wd4625 /wd4626 /wd4514 /wd26472 /wd26821)
