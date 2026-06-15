cmake_minimum_required(VERSION 3.10)

project(great-hole VERSION 0.1.0 LANGUAGES CXX C)

option(GREAT_HOLE_RELATIVE_PATHS "Use relative paths" OFF)
option(BUILD_WITH_LIBCXX "Build with libc++" OFF)
option(BUILD_TESTING "Build unit tests" ON)
option(GREAT_HOLE_BUILD_ANDROID "Build Android JNI library" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_THREAD_LIBS_INIT "-lpthread")

if(BUILD_WITH_LIBCXX)
  add_compile_options(-stdlib=libc++)
  add_link_options(-stdlib=libc++)
endif()

if(GREAT_HOLE_BUILD_ANDROID)
  if(NOT DEFINED ANDROID_NDK)
    message(FATAL_ERROR "ANDROID_NDK is not defined")
  endif()
  if(NOT DEFINED ANDROID_ABI)
    message(FATAL_ERROR "ANDROID_ABI is not defined")
  endif()
  if(NOT DEFINED ANDROID_PLATFORM)
    message(FATAL_ERROR "ANDROID_PLATFORM is not defined")
  endif()

  set(CMAKE_TOOLCHAIN_FILE "${ANDROID_NDK}/build/cmake/android.toolchain.cmake" CACHE FILEPATH "Android toolchain" FORCE)
  # Search paths for dependencies (c-ares, Boost) built by build_deps.sh
  set(CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/build-android-${ANDROID_ABI}/dependencies/install" CACHE PATH "Prefix path for dependencies" FORCE)
  set(CMAKE_FIND_ROOT_PATH "${CMAKE_SOURCE_DIR}/build-android-${ANDROID_ABI}/dependencies/install" CACHE PATH "Find root path for dependencies" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH CACHE STRING "Find root path mode package" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH CACHE STRING "Find root path mode library" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH CACHE STRING "Find root path mode include" FORCE)
endif()

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# Dependencies
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED COMPONENTS log program_options)

if(ANDROID OR GREAT_HOLE_BUILD_ANDROID)
  # Find CARES manually
  find_path(CARES_INCLUDE_DIR NAMES ares.h)
  find_library(CARES_LIBRARY NAMES cares)
  add_library(PkgConfig::CARES INTERFACE IMPORTED)
  set_target_properties(PkgConfig::CARES PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CARES_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${CARES_LIBRARY}"
  )
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua-5.4)
  pkg_check_modules(CARES REQUIRED IMPORTED_TARGET libcares)
endif()

if(BUILD_TESTING)
  enable_testing()
  if(BUILD_WITH_LIBCXX)
    if(NOT TARGET gtest)
      add_subdirectory("/usr/src/googletest" "${CMAKE_BINARY_DIR}/googletest-build" EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET GTest::gtest)
      add_library(GTest::gtest ALIAS gtest)
    endif()
    if(NOT TARGET GTest::gtest_main)
      add_library(GTest::gtest_main ALIAS gtest_main)
    endif()
  else()
    find_package(GTest REQUIRED)
  endif()
endif()

if(GREAT_HOLE_RELATIVE_PATHS)
  add_compile_options("-ffile-compilation-dir=.")
  add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=./")
endif()


set(OMNIFIBER_INTERNAL ON CACHE BOOL "" FORCE)

add_subdirectory(libs/omni-fiber)
add_subdirectory(src)
