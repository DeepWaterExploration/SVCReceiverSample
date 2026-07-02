cmake_minimum_required(VERSION 3.10)
project(SVCReceiverSample LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(thirdparty/uvgrtp)

add_executable(${PROJECT_NAME}
    src/sample.cc
        src/sample.cc
)

target_link_libraries(${PROJECT_NAME} uvgrtp)
