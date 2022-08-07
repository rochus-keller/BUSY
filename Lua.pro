QT       -= core
QT       -= gui

TARGET = Lua
CONFIG   += console
CONFIG   -= app_bundle

TEMPLATE = app

QMAKE_CFLAGS += -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-but-set-parameter

HEADERS += \
    lapi.h \
    lauxlib.h \
    lcode.h \
    ldebug.h \
    ldo.h \
    lfunc.h \
    lgc.h \
    llex.h \
    llimits.h \
    lmem.h \
    lobject.h \
    lopcodes.h \
    lparser.h \
    lstate.h \
    lstring.h \
    ltable.h \
    ltm.h \
    lua.h \
    luaconf.h \
    lualib.h \
    lundump.h \
    lvm.h \
    lzio.h 

SOURCES += \
    lapi.c \
    lauxlib.c \
    lbaselib.c \
    lcode.c \
    ldblib.c \
    ldebug.c \
    ldo.c \
    ldump.c \
    lfunc.c \
    lgc.c \
    linit.c \
    liolib.c \
    llex.c \
    lmathlib.c \
    lmem.c \
    loadlib.c \
    lobject.c \
    lopcodes.c \
    loslib.c \
    lparser.c \
    lstate.c \
    lstring.c \
    lstrlib.c \
    ltable.c \
    ltablib.c \
    ltm.c \
    lua.c \
    lundump.c \
    lvm.c \
    lzio.c \
    print.c 




