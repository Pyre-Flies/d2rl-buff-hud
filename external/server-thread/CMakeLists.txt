# Header-only: the d2rds "server-thread" service table, shared by its provider
# (plugins/d2rds) and its consumers (plugins/identify-on-click).
add_library(D2RLServerThread INTERFACE)
add_library(D2RL::ServerThread ALIAS D2RLServerThread)
target_include_directories(D2RLServerThread INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
