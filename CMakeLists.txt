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
    add_executable(client_command_exchange_development_test
        tests/lifecycle_command_transaction_test.cpp
        src/grpc/grpc_client.cpp
        src/env/maze_env.cpp
        ${PROTO_SOURCES}
    )
    target_include_directories(client_command_exchange_development_test PRIVATE
        ${COMMON_INCLUDES}
    )
    target_link_libraries(client_command_exchange_development_test PRIVATE
        ${COMMON_LIBS}
    )
    add_test(
        NAME client_command_exchange_development_contract
        COMMAND client_command_exchange_development_test
            ${CMAKE_CURRENT_SOURCE_DIR}/maps/test/maze_117436372.json
    )
endif()
