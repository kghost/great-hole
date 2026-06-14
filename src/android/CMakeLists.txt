# CMakeLists.txt for Android JNI Tunnel Data Plane Native Library

add_library(androidhole_dataplane SHARED
  JniTunnelDataPlaneNative.cpp
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

target_compile_definitions(androidhole_dataplane PRIVATE
  BOOST_ASIO_ENABLE_OLD_SERVICES
  BOOST_ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
  BOOST_ASIO_HAS_MOVE
)

target_link_libraries(androidhole_dataplane PRIVATE
  great-hole-application
  great-hole-core
  omni-fiber
  log
)
