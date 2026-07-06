cmake_minimum_required(VERSION 3.10)
project(SVCReceiverSample LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(thirdparty/uvgrtp)
add_subdirectory(thirdparty/httplib)
add_subdirectory(thirdparty/json)

add_executable(${PROJECT_NAME}
    src/sample.cc
)

target_include_directories(${PROJECT_NAME} PRIVATE thirdparty/stb thirdparty/fp16/include thirdparty/happly)

target_link_libraries(${PROJECT_NAME} uvgrtp httplib nlohmann_json::nlohmann_json)
