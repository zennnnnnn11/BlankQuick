blank_library(profile_storage profile_storage.cpp profile_digest.cpp)
target_link_libraries(profile_storage PUBLIC profile_types PRIVATE profile_codec node_input Qt6::Core bcrypt)
set_property(SOURCE profile_digest.cpp APPEND PROPERTY COMPILE_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN)
