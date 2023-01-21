#ifndef BSPARSER_H
#define BSPARSER_H

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
#include "bslogger.h"

typedef enum BSNodeType { // #kind
    BS_Invalid,
    BS_BaseType, // see BSBaseType
    BS_ListType,
    BS_ModuleDef,
    BS_ClassDecl,
    BS_EnumDecl,
    BS_VarDecl,
    BS_FieldDecl,
    BS_BlockDef,
    BS_ProcDef,
    BS_MacroDef,
    BS_CondStat,
} BSNodeType;

typedef enum BSBaseType { // #type
    BS_nil,
    BS_boolean,
    BS_integer,
    BS_real,
    BS_string,
    BS_path,
    BS_symbol
} BSBaseType;

typedef enum BSVisibility { // #visi
    BS_Private, BS_Protected, BS_Public, BS_PublicDefault
} BSVisibility;

typedef enum BSReadability { // #rw
    BS_var, BS_let, BS_param
} BSReadability;



extern void bs_preset_logger(lua_State *L, BSLogger, void*);


/* Expects
 * 1. an absolute, normalized path to the directory in which BUSY is present,
 * 2. the new module def table which is filled by bs_parser, kind and outer already set,
 * 3. and the parameter table.
 * Returns a module definition as a Lua table on the Lua stack.
 * The actual module instance carrying the data is in another table which is referenced in the definition by #inst.
 *
 * The AST (but expressions and statements) is mapped to Lua tables in general.
 * There is a table for vardecl, enumtype, basictype, classtype, fielddecl; expressions and statements are
 *   immediately executed.
 * Class and list instances are tables as well; basic types are directly mapped to Lua values.
 */
extern int bs_parse(lua_State *L);

// supporting features
extern int bs_add_path(lua_State* L, int lhs, int rhs); // return 0..ok, >0..error
extern int bs_isa( lua_State *L, int lhs, int rhs ); // returns 1 if rhs is same or subclass of lhs, 0 otherwise
extern unsigned int bs_torowcol(int row, int col);
extern unsigned int bs_torow(int rowcol);
extern unsigned int bs_tocol(int rowcol);

// debugging feature; expects zero or more Lua values
extern int bs_dump(lua_State *L);
extern void bs_dump2(lua_State *L, const char* title, int index );

#endif // BSPARSER_H
