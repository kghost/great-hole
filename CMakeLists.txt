cmake_minimum_required(VERSION 3.10)
project(great-hole VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_THREAD_LIBS_INIT "-lpthread")

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# Dependencies
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED COMPONENTS log program_options)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua-5.4)

enable_testing()
find_package(GTest REQUIRED)

add_compile_options("-ffile-compilation-dir=.")
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=")
set(OMNIFIBER_INTERNAL ON CACHE BOOL "" FORCE)

add_subdirectory(libs/omni-fiber)
add_subdirectory(src)
