lib_LTLIBRARIES = liblua.la

liblua_la_SOURCES = lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c lbitlib.c lcorolib.c ldblib.c liolib.c lmathlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c loadlib.c linit.c

liblua_la_CFLAGS = -x c++ -DLUA_USE_LINUX -DLUA_USER_H=\"src/lua-ext.h\"
liblua_la_LDFLAGS = -ldl
