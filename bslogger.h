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

typedef enum { BS_Info, BS_Debug, BS_Warning, BS_Error, BS_Critical } BSLogLevel;
typedef void (*BSLogger)(BSLogLevel, void* data, const char* file, BSRowCol loc, const char* format, va_list);
                // file & loc.row may be 0; format doesn't require terminal \n

#endif // BSLOGGER_H

