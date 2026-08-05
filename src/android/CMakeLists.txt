# CMakeLists.txt for Android JNI Tunnel Data Plane Native Library

add_library(androidhole_dataplane SHARED
  JniTunnelDataPlaneNative.cpp
)

set_target_properties(androidhole_dataplane PROPERTIES
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(androidhole_dataplane PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/base
  ${CMAKE_SOURCE_DIR}/src/core
  ${CMAKE_SOURCE_DIR}/src/application
  ${CMAKE_SOURCE_DIR}/src/resolvers
  ${CMAKE_SOURCE_DIR}/libs/omni-fiber
)

target_link_options(androidhole_dataplane PRIVATE
  "-Wl,--gc-sections"
  "-Wl,--exclude-libs,ALL"
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/androidhole_dataplane.map"
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_link_options(androidhole_dataplane PRIVATE "-Wl,--strip-all")
endif()

target_link_libraries(androidhole_dataplane PRIVATE
  great-hole-application
  great-hole-core
  omni-fiber
  log
)
