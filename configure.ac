dnl Process this file with autoconf to produce a configure script.

AC_CONFIG_MACRO_DIR([m4])

AC_INIT(great-hole, 0.1.0)
AC_CONFIG_SRCDIR([src/main.cpp])
LT_INIT
AM_INIT_AUTOMAKE
AM_CONFIG_HEADER(config.h)

AC_SUBST(VERSION)

ISODATE=`date +%Y-%m-%d`
AC_SUBST(ISODATE)

AC_CANONICAL_HOST

AC_PROG_INSTALL
AC_PROG_CC(clang gcc)
AC_PROG_CPP(clang++ g++)
AC_PROG_CXX(clang++ g++)
AX_CXX_COMPILE_STDCXX_14

AC_CHECK_TOOL([OBJCOPY], [objcopy])

AC_DEFINE([BOOST_ASIO_HAS_MOVE], [1], [Enable asio move handler])

dnl Checks for libraries.

dnl Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS(unistd.h sys/param.h sys/time.h time.h sys/mkdev.h sys/sysmacros.h string.h memory.h fcntl.h dirent.h sys/ndir.h ndir.h alloca.h locale.h )

AC_HEADER_MAJOR
AC_FUNC_ALLOCA
AC_STRUCT_TM
AC_STRUCT_ST_BLOCKS
AC_FUNC_CLOSEDIR_VOID

AX_PTHREAD

AX_BOOST_BASE
AX_BOOST_SYSTEM
AX_BOOST_LOG
AX_BOOST_IOSTREAMS
AX_BOOST_PROGRAM_OPTIONS
AX_BOOST_ASIO
AX_BOOST_FILESYSTEM
AX_BOOST_THREAD

AC_CHECK_LIB(zlib, zlib)

dnl Checks for typedefs, structures, and compiler characteristics.

dnl Checks for library functions.

AC_OUTPUT([Makefile src/Makefile libs/Makefile libs/lua-5.3.2/Makefile])
