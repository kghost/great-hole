foreach(suffix "" "-asan")
  if(suffix STREQUAL "-asan" AND SKIP_ASAN)
    continue()
  endif()
  if(suffix STREQUAL "" AND SKIP_NON_ASAN)
    continue()
  endif()

  set(target "net-interface-dump${suffix}")

  add_executable(${target} InterfaceDump.cpp)

  target_link_libraries(${target} PRIVATE
    great-hole-windows-network${suffix}
    great-hole-windows-utils${suffix}
    Boost::log
  )

  set(dns_target "net-dns-tool${suffix}")

  add_executable(${dns_target} DnsTool.cpp)

  target_link_libraries(${dns_target} PRIVATE
    great-hole-windows-network${suffix}
    great-hole-windows-utils${suffix}
    Boost::log
  )

  if(MSVC)
    target_link_options(${target} PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
    target_link_options(${dns_target} PRIVATE "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
  endif()

  if(suffix STREQUAL "-asan")
    target_compile_options(${target} PRIVATE -fsanitize=address)
    target_link_options(${target} PRIVATE -fsanitize=address)
    target_compile_options(${dns_target} PRIVATE -fsanitize=address)
    target_link_options(${dns_target} PRIVATE -fsanitize=address)
  endif()
endforeach()
