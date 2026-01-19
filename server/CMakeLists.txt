cmake_minimum_required(VERSION 3.16)
project(cube_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0)
pkg_check_modules(LWS REQUIRED libwebsockets)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)

add_executable(cube_server src/main.c src/json_util.c)

target_include_directories(cube_server PRIVATE
  ${GST_INCLUDE_DIRS}
  ${LWS_INCLUDE_DIRS}
  ${EGL_INCLUDE_DIRS}
  ${GLES2_INCLUDE_DIRS}
)

target_compile_options(cube_server PRIVATE
  ${GST_CFLAGS_OTHER}
  ${LWS_CFLAGS_OTHER}
  ${EGL_CFLAGS_OTHER}
  ${GLES2_CFLAGS_OTHER}
)

target_link_libraries(cube_server PRIVATE
  ${GST_LIBRARIES}
  ${LWS_LIBRARIES}
  ${EGL_LIBRARIES}
  ${GLES2_LIBRARIES}
  m
  pthread
)

enable_testing()

add_executable(test_json tests/test_json.c src/json_util.c)
target_include_directories(test_json PRIVATE src)
add_test(NAME json_utils COMMAND test_json)
