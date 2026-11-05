blank_library(override_script override_script.cpp)
target_link_libraries(override_script PUBLIC overrides PRIVATE Qt6::Qml)
