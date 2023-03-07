#ifndef BSLOGGER_H
#define BSLOGGER_H

/*
* Copyright 2023 Rochus Keller <mailto:me@rochus-keller.ch>
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

#include <stdarg.h>

typedef struct BSRowCol
{
    unsigned int row: 20;
    unsigned int col: 12;
} BSRowCol;

/////// Logger:
typedef enum { BS_Info, BS_Debug, BS_Message, BS_Warning, BS_Error, BS_Critical } BSLogLevel;
typedef void (*BSLogger)(BSLogLevel, void* data, const char* file, BSRowCol loc, const char* format, va_list);
                // file & loc.row may be 0; format doesn't require terminal \n

/////// Visitor:
typedef enum BSBuildOperation { BS_Compile,
                                BS_LinkExe, BS_LinkDll, BS_LinkLib,
                                BS_RunMoc, BS_RunRcc, BS_RunUic, BS_RunLua,
                                BS_Copy,
                                BS_EnteringProduct // just to inform where we are, no BSEndOp sent
                              } BSBuildOperation;

typedef enum BSBuildParam { BS_infile, BS_outfile,
                            BS_cflag, BS_define, BS_include_dir,
                            BS_ldflag, BS_lib_dir, BS_lib_name, BS_lib_file, BS_framework,
                            BS_defFile, BS_name, BS_arg,
                          } BSBuildParam;

// toolchain : BSToolchain
// os : BSOperatingSystem
typedef int (*BSBeginOp)(BSBuildOperation, const char* command, int toolchain, int os, void* data); // returns 0 if ok or -1 if cancel
typedef void (*BSOpParam)(BSBuildParam, const char* value, void* data);
typedef void (*BSEndOp)(void* data);
typedef void (*BSForkGroup)(int n, void* data); // n >= 0: start group, < 0: end group

#endif // BSLOGGER_H

