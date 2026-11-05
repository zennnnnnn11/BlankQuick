blank_library(overrides override_document.cpp override_engine.cpp)
target_link_libraries(overrides PUBLIC blank_shared PRIVATE rapidyaml Qt6::Core)
add_library(BlankQuick::Overrides ALIAS overrides)
