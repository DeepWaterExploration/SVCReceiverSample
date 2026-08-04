cmake_minimum_required(VERSION 3.10)
project(SVCReceiverSample LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(thirdparty/uvgrtp)
add_subdirectory(thirdparty/httplib)
add_subdirectory(thirdparty/json)

add_executable(left_depth src/sample_left_depth.cc)
target_include_directories(left_depth PRIVATE thirdparty/stb thirdparty/fp16/include thirdparty/happly)
target_link_libraries(left_depth uvgrtp httplib nlohmann_json::nlohmann_json)


add_executable(left_right_depth src/sample_left_right_depth.cc)
target_include_directories(left_right_depth PRIVATE thirdparty/stb thirdparty/fp16/include thirdparty/happly)
target_link_libraries(left_right_depth uvgrtp httplib nlohmann_json::nlohmann_json)
