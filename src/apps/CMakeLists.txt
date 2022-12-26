cmake_minimum_required(VERSION 3.6)

project(libcamera-server)
add_executable(libcamera-server libcamera_server.cpp)
target_link_libraries(libcamera-server libcamera_app encoders outputs)

set(EXECUTABLES libcamera-server)

set(EXECUTABLE_OUTPUT_PATH  ${CMAKE_BINARY_DIR})
install(TARGETS ${EXECUTABLES} RUNTIME DESTINATION bin)
