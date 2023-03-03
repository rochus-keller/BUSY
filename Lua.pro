#* Copyright 2022 Rochus Keller <mailto:me@rochus-keller.ch>
#*
#* This file is part of the BUSY build system.
#*
#* The following is the license that applies to this copy of the
#* application. For a license to use the application under conditions
#* other than those described here, please email to me@rochus-keller.ch.
#*
#* GNU General Public License Usage
#* This file may be used under the terms of the GNU General Public
#* License (GPL) versions 2.0 or 3.0 as published by the Free Software
#* Foundation and appearing in the file LICENSE.GPL included in
#* the packaging of this file. Please review the following information
#* to ensure GNU General Public Licensing requirements will be met:
#* http://www.fsf.org/licensing/licenses/info/GPLv2.html and
#* http://www.gnu.org/copyleft/gpl.html.

QT       -= core
QT       -= gui

TARGET = lua
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
    lzio.h \
    bslex.h \
    bsparser.h \
    bsunicode.h \
    bslib.h \
    bsrunner.h \
    bshost.h \
    bsdetect.h \
    bsqmakegen.h \
    bsvisitor.h \
    bscallbacks.h

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
    print.c \
    bslex.c \
    bsparser.c \
    bsunicode.c \
    bslib.c \
    bsrunner.c \
    bshost.c \
    bsqmakegen.c \
    bsvisitor.c




