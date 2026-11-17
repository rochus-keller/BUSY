/*
* Copyright 2026 Rochus Keller <mailto:me@rochus-keller.ch>
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

#ifndef BSEXEC_H
#define BSEXEC_H

#include "lua.h"
#include <time.h>

LUALIB_API int bs_open_bsexec(lua_State* L);

int bs_os_getcwd(char* path, int len);
int bs_os_app_path(char* path, int len);
int bs_os_mtime(const char* path, time_t* result);
int bs_os_touch(const char* path);
int bs_os_path_max(void);
int bs_mkdir2(const char* path);
int bs_spawn(const char* const* argv, const char* cwd, const char* in,
             const char* out, const char* err);
int bs_system(const char* cmd);
int bs_mkdirp(const char* path);
int bs_exists3(const char* path);
int bs_copyfile(const char* to, const char* from);
int bs_copytree(const char* to, const char* from);
int bs_listdir(const char* dir, void (*cb)(const char* name, void* data),
               void* data);

#endif
