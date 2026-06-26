include(FetchContent)

FetchContent_Declare(
  windivert
  URL "https://github.com/basil00/Divert/releases/download/v2.2.2/WinDivert-2.2.2-A.zip"
)
FetchContent_MakeAvailable(windivert)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WINDIVERT_ARCH "x64")
  set(WINDIVERT_SYS "WinDivert64.sys")
else()
  set(WINDIVERT_ARCH "x86")
  set(WINDIVERT_SYS "WinDivert32.sys")
endif()

add_library(windivert_lib INTERFACE)
target_include_directories(windivert_lib INTERFACE "${windivert_SOURCE_DIR}/include")
target_link_libraries(windivert_lib INTERFACE "${windivert_SOURCE_DIR}/${WINDIVERT_ARCH}/WinDivert.lib")
