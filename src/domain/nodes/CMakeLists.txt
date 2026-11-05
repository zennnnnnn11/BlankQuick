blank_library(node_model unified_node.cpp)
blank_library(node_input node_input.cpp node_input_bindings.inc node_input_document.inc
    node_input_yaml.inc node_input_singbox.inc node_input_wireguard.inc)
target_link_libraries(node_input PUBLIC node_model blank_shared PRIVATE rapidyaml)
add_library(BlankQuick::Nodes ALIAS node_input)
