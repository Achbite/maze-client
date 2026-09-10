cmake_minimum_required(VERSION 3.16)
project(maze_client_runtime LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(proto/rl_sdk)
rl_sdk_add_task(maze_task_protocol "${CMAKE_CURRENT_SOURCE_DIR}" proto/maze/maze)

add_library(maze_environment STATIC
    src/maze/environment/maze_env.cpp
    src/maze/config/config_loader.cpp
    src/maze/viz/viz_recorder.cpp)
target_include_directories(maze_environment PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(maze_client_adapter STATIC src/maze/protocol/client_adapter.cpp)
target_link_libraries(maze_client_adapter PUBLIC maze_environment maze_task_protocol)
add_executable(maze_client main/main.cpp)
target_link_libraries(maze_client PRIVATE maze_client_adapter)

include(CTest)
if(BUILD_TESTING)
    add_executable(client_command_exchange_development_test
        tests/lifecycle_command_transaction_test.cpp)
    target_link_libraries(client_command_exchange_development_test PRIVATE
        maze_client_adapter)
    add_test(NAME client_command_exchange_data_path
        COMMAND client_command_exchange_development_test
            ${CMAKE_CURRENT_SOURCE_DIR}/maps/test/maze_117436372.json)
endif()
