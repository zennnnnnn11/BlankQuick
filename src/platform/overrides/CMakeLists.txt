blank_library(override_storage override_manager.cpp)
target_link_libraries(override_storage PUBLIC overrides PRIVATE Qt6::Core)
