#ifndef BSLIB_H
#define BSLIB_H

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

#define BS_BSLIBNAME	"BUSY"
#define BS_BSVERSION    "2023-03-03"

LUALIB_API int bs_open_busy (lua_State *L);

LUALIB_API int bs_compile (lua_State *L);
// opt param: path to source root directory (default '..')
// opt param: path to output root dir (default ./output)
// opt param: table with parameter values
// returns: root module

LUALIB_API int bs_execute (lua_State *L);
// param: root module def
// opt param: set of product desigs to be built

LUALIB_API int bs_findProductsToProcess(lua_State *L);
// param: root module returned by bs_compile
// param: table where the keys are the product desigs, or nil (in which case all ! are searched)
// param: builtins
// returns an array of product insts to be built

#endif // BSLIB_H
