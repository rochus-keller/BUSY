#ifndef BSRUNNER_H
#define BSRUNNER_H

/*
* Copyright 2022 Rochus Keller <mailto:me@rochus-keller.ch>
*
* This file is part of the BUSY build system.
*
* The following is the license that applies to this copy of the
* application. For a license to use the application under conditions
* other than those described here, please email to me@rochus-keller.ch.
*
* GNU General Public License Usage
* This file may be used under the terms of the GNU General Public
* License (GPL) versions 2.0 or 3.0 as published by the Free Software
* Foundation and appearing in the file LICENSE.GPL included in
* the packaging of this file. Please review the following information
* to ensure GNU General Public Licensing requirements will be met:
* http://www.fsf.org/licensing/licenses/info/GPLv2.html and
* http://www.gnu.org/copyleft/gpl.html.
*/

#include "lua.h"

typedef enum BSOutKind { // #kind
    BS_Nothing,
    BS_Mixed, // list of list of the other kinds
    BS_ObjectFiles,
    BS_StaticLib,
    BS_DynamicLib,
    BS_Executable,
    BS_SourceFiles, // in case of Moc
#if 0
    BS_IncludeFiles // in case of Moc
#endif
    BS_SourceSetLib // in case of qmake gen, static lib representing the sources
} BSOutKind;

extern int bs_run(lua_State* L);
extern int bs_precheck(lua_State* L);
extern int bs_markActive(lua_State* L); // params: productinst, array of decls in exec order,
extern int bs_markAllActive(lua_State* L); // params: array of productinst, array of decls in exec order,
extern int bs_createBuildDirs(lua_State* L);
extern int bs_thisapp2(lua_State *L); // pushes a path on stack or throws error

// helper:
extern int bs_getModuleVar(lua_State* L, int inst, const char* name );
extern int bs_declpath(lua_State* L, int decl, const char* separator);
extern int bs_runmoc(lua_State* L);
extern int bs_mocname(lua_State* L);

typedef enum BSLanguage { BS_unknownLang, BS_c, BS_cc, BS_objc, BS_objcc, BS_header } BSLanguage;
extern int bs_guessLang(const char* name);

typedef enum BSToolchain {BS_notc, BS_msvc,BS_gcc,BS_clang} BSToolchain;
extern BSToolchain bs_getToolchain(lua_State* L, int builtinsInst, int to_host);

typedef enum BSOperatingSystem {BS_noos, BS_linux,BS_mac,BS_windows} BSOperatingSystem;
extern BSOperatingSystem bs_getOperatingSystem(lua_State* L, int builtinsInst, int to_host);


#ifdef BS_ALT_RUNCMD
typedef int (*BSRunCmd)(const char* cmd, void* data);
extern void bs_preset_runcmd(lua_State *L, BSRunCmd, void*);
#endif

#endif // BSRUNNER_H
