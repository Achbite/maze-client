cmake_minimum_required(VERSION 3.16)
project(maze_client_runtime LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 依赖查找 ---
find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED grpc++)

set(SHARED_SOURCES
    src/grpc/grpc_client.cpp
    src/env/maze_env.cpp
    src/config/config_loader.cpp
    src/viz/viz_recorder.cpp
)

set(CONTRACT_CPP_DIR "" CACHE PATH "Generated C++ contract directory")
if(CONTRACT_CPP_DIR)
    set(PROTO_GEN_DIR ${CONTRACT_CPP_DIR})
else()
    set(PROTO_GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
endif()
set(PROTO_SOURCES
    ${PROTO_GEN_DIR}/common.pb.cc
    ${PROTO_GEN_DIR}/maze_task.pb.cc
    ${PROTO_GEN_DIR}/maze_task.grpc.pb.cc
)

# --- 公共头文件搜索路径 ---
set(COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROTO_GEN_DIR}
    ${Protobuf_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
)

# --- 公共链接库 ---
set(COMMON_LIBS
    protobuf::libprotobuf
    ${GRPC_LIBRARIES}
    pthread
    crypto
)

add_executable(maze_client main/main.cpp ${SHARED_SOURCES} ${PROTO_SOURCES})
target_include_directories(maze_client PRIVATE ${COMMON_INCLUDES})
target_link_libraries(maze_client PRIVATE ${COMMON_LIBS})

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME client_cli_contract
        COMMAND python3
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_contract.py
            $<TARGET_FILE:maze_client>
    )

    add_executable(client_config_loader_test
        tests/config_loader_test.cpp
        src/config/config_loader.cpp
    )
    target_include_directories(client_config_loader_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME client_config_loader_contract
             COMMAND client_config_loader_test
                     ${CMAKE_CURRENT_SOURCE_DIR}/configs/client_config.yaml)

    add_executable(maze_env_contract_test
        tests/maze_env_contract_test.cpp
        src/env/maze_env.cpp
    )
    target_include_directories(maze_env_contract_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(maze_env_contract_test PRIVATE crypto)
    add_test(NAME maze_env_contract
             COMMAND maze_env_contract_test
                     ${CMAKE_CURRENT_SOURCE_DIR}/maps/test/maze_117436372.json)

    add_test(
        NAME replay_mode_contract
        COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_mode_test.sh
    )
    add_executable(update_flow_control_test
        tests/update_flow_control_test.cpp
        ${PROTO_GEN_DIR}/common.pb.cc
        ${PROTO_GEN_DIR}/maze_task.pb.cc
    )
    target_include_directories(update_flow_control_test PRIVATE
        ${COMMON_INCLUDES}
    )
    target_link_libraries(update_flow_control_test PRIVATE
        protobuf::libprotobuf
    )
    add_test(NAME update_flow_control_contract
             COMMAND update_flow_control_test)

    add_executable(lifecycle_command_transaction_test
        tests/lifecycle_command_transaction_test.cpp
        ${PROTO_GEN_DIR}/common.pb.cc
        ${PROTO_GEN_DIR}/maze_task.pb.cc
    )
    target_include_directories(lifecycle_command_transaction_test PRIVATE
        ${COMMON_INCLUDES}
    )
    target_link_libraries(lifecycle_command_transaction_test PRIVATE
        protobuf::libprotobuf
    )
    add_test(NAME lifecycle_command_transaction_contract
             COMMAND lifecycle_command_transaction_test)
endif()
