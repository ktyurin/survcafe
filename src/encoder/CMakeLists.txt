cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

set(SRC encoder.cpp null_encoder.cpp h264_encoder.cpp mjpeg_encoder.cpp)
set(TARGET_LIBS images)

#pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET ffspeg)
pkg_check_modules(LIBAV QUIET IMPORTED_TARGET
    libavcodec
    libavdevice
    libavformat
    libswresample
)

add_library(encoders ${SRC})
target_link_libraries(encoders ${TARGET_LIBS})
target_compile_definitions(encoders PUBLIC)

install(TARGETS encoders LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

