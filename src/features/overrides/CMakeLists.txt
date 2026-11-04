feature_library(overrides override_document.cpp override_engine.cpp override_script.cpp override_manager.cpp)
target_link_libraries(overrides PUBLIC blank_shared PRIVATE rapidyaml Qt6::Qml)
add_library(BlankQuick::Overrides ALIAS overrides)
