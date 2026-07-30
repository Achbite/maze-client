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
    ${PROTO_GEN_DIR}/maze.pb.cc
    ${PROTO_GEN_DIR}/maze.grpc.pb.cc
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
)

add_executable(maze_client main/main.cpp ${SHARED_SOURCES} ${PROTO_SOURCES})
target_include_directories(maze_client PRIVATE ${COMMON_INCLUDES})
target_link_libraries(maze_client PRIVATE ${COMMON_LIBS})

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME replay_mode_contract
        COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_mode_test.sh
    )
endif()
