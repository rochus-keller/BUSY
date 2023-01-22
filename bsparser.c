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

// It's actually not only a parser, but it also directly interprets the syntax with no full AST.

#include "bsparser.h"
#include "bslex.h" 
#include "bshost.h"
#include "bsunicode.h"
#include <memory.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "lauxlib.h"
#include <stdarg.h>

typedef enum BS_ParseArgs { BS_PathToSourceRoot = 1, BS_NewModule, BS_Params } BS_ParseArgs;

typedef struct BSScope {
    int table; // index to table on the stack
    int n; // number of ordered items
} BSScope;

typedef struct BSParserContext {
    BSHiLex* lex;
    BSScope module;
    const char* dirpath;  // normalized absolute path to current dir
    const char* label; // pointer to internal of dirpath to display in error messages
    const char* filepath; // normalized absolute path to BUSY in current dir
    unsigned builtins : 8;
    unsigned skipMode : 1; // when on, read over tokens without executing
    unsigned locInfo : 1;  // when on, add #row, #col and #file to AST elements
    unsigned fullAst : 1;  // when on, also add AST statement and expression elements
    unsigned numRefs : 8;  // optional index a weak #refs table pointing to all objects with identdef
    unsigned xref : 8;     // optional index to xref table or 0; if a table is present, build bi-dir xref
    lua_State* L;
    BSLogger logger;
    void* loggerData;
} BSParserContext;

static BSToken nextToken(BSParserContext* ctx)
{
    return bslex_hnext(ctx->lex);
}

static BSToken peekToken(BSParserContext* ctx, int off ) // off = 1..
{
    return bslex_hpeek(ctx->lex,off);
}

typedef struct BSIdentDef {
    const char* name;
    int len;
    int visi; // BSVisibility
    BSRowCol loc;
} BSIdentDef;

static void printLexerStack(BSParserContext* ctx)
{
    int i = bslex_hlevelcount(ctx->lex) - 2;
    while(i >= 0)
    {
        BSToken level = bslex_hlevel(ctx->lex,i);
        BSRowCol loc;
        loc.row = level.loc.row;
        loc.col = level.loc.col;
        va_list args;
        va_end(args);
        ctx->logger(BS_Error, ctx->loggerData,level.source,loc, "    instantiated from here", args );
        i--;
    }
}

static void error(BSParserContext* ctx, int row, int col, const char* format, ... )
{
    BSRowCol loc;
    loc.row = row;
    loc.col = col;
    va_list args;
    va_start(args, format);
    ctx->logger(BS_Error, ctx->loggerData,bslex_hfilepath(ctx->lex),loc, format, args );
    va_end(args);
    printLexerStack(ctx);
    lua_pushnil(ctx->L);
    lua_error(ctx->L);
}

static void warning(BSParserContext* ctx, int row, int col, const char* format, ... )
{
    BSRowCol loc;
    loc.row = row;
    loc.col = col;
    va_list args;
    va_start(args, format);
    ctx->logger(BS_Warning, ctx->loggerData,bslex_hfilepath(ctx->lex),loc, format, args );
    va_end(args);
}

static void message(BSParserContext* ctx, const char* format, ... )
{
    BSRowCol loc;
    loc.row = 0;
    loc.col = 0;
    va_list args;
    va_start(args, format);
    ctx->logger(BS_Info, ctx->loggerData,0,loc, format, args );
    va_end(args);
}

static void debugprint(BSLogger out, void* data, const char* format, ... )
{
    BSRowCol loc;
    loc.row = 0;
    loc.col = 0;
    va_list args;
    va_start(args, format);
    out(BS_Debug, data,0,loc, format, args );
    va_end(args);
}

static void unexpectedToken(BSParserContext* ctx, BSToken* t, const char* where)
{
    error(ctx,t->loc.row, t->loc.col,"unexpected token %s: %s", where, bslex_tostring(t->tok) );
}

#define BS_BEGIN_LUA_FUNC(ctx,diff) const int $stack = lua_gettop((ctx)->L) + diff
#define BS_END_LUA_FUNC(ctx) int $end = lua_gettop((ctx)->L); assert($stack == $end)

static void dumpimp(lua_State* L, int index, BSLogger out, void* data, const char* title)
{
    if( index <= 0 )
        index += lua_gettop(L) + 1;
    if( title && *title != 0 )
        debugprint(out,data,title);
    switch( lua_type(L,index) )
    {
    case LUA_TNIL:
        debugprint(out,data,"nil");
        break;
    case LUA_TBOOLEAN:
        debugprint(out,data,"bool %d",lua_toboolean(L,index));
        break;
    case LUA_TNUMBER:
        if( ( (int)lua_tonumber(L,index) - lua_tonumber(L,index) ) == 0.0 )
            debugprint(out,data,"int %d",lua_tointeger(L,index));
        else
            debugprint(out,data,"number %f",lua_tonumber(L,index));
        break;
    case LUA_TSTRING:
        debugprint(out,data,"string \"%s\"",lua_tostring(L,index));
        break;
    case LUA_TTABLE:
        debugprint(out,data,"*** table: %p", lua_topointer(L,index));
        if( lua_getmetatable(L,index) )
        {
            debugprint(out,data,"  metatable %p", lua_topointer(L,-1));
            lua_pop(L,1);
        }
        lua_pushnil(L);  /* first key */
        while (lua_next(L, index) != 0) {
            const int key = lua_gettop(L)-1;
            const int value = lua_gettop(L);
            if( lua_type(L,key) == LUA_TTABLE )
                lua_pushfstring(L,"table %p", lua_topointer(L,key));
            else
                lua_pushvalue(L,key);
            if( lua_type(L,value) == LUA_TTABLE )
                lua_pushfstring(L,"table %p", lua_topointer(L,value));
            else
                lua_pushvalue(L,value);
            debugprint(out,data,"  %s = %s", lua_tostring(L,-2), lua_tostring(L,-1));
            lua_pop(L, 3);
        }
        break;
    case LUA_TFUNCTION:
        debugprint(out,data,"function %p",lua_topointer(L,index));
        break;
    default:
        debugprint(out,data,"<lua value>");
        break;
    }
}


static void dump(BSParserContext* ctx, int index)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    dumpimp(ctx->L,index,ctx->logger,ctx->loggerData,0);
    BS_END_LUA_FUNC(ctx);
}

static void dump2(BSParserContext* ctx, const char* title, int index)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    dumpimp(ctx->L,index,ctx->logger,ctx->loggerData,title);
    BS_END_LUA_FUNC(ctx);
}


static void checkUnique(BSParserContext* ctx, BSScope* scope, BSIdentDef* id )
{
    lua_pushlstring(ctx->L,id->name,id->len);
    const char* name = lua_tostring(ctx->L,-1);
    lua_rawget(ctx->L,scope->table);
    const int isnil = lua_isnil(ctx->L,-1);
    lua_pop(ctx->L,1);
    if( !isnil )
        error(ctx,id->loc.row, id->loc.col,"name is not unique in scope: '%s'", name );
}

static void addLocInfo(BSParserContext* ctx, BSRowCol loc, int table)
{
    if( ctx->locInfo )
    {
        lua_pushinteger(ctx->L, loc.row );
        lua_setfield(ctx->L, table, "#row");
        lua_pushinteger(ctx->L, loc.col );
        lua_setfield(ctx->L, table, "#col");
        lua_getfield(ctx->L, ctx->module.table,"#file");
        lua_setfield(ctx->L,table,"#file");
    }
}

enum { ROW_BIT_LEN = 19, COL_BIT_LEN = 32 - ROW_BIT_LEN };
unsigned int bs_torowcol(int row, int col)
{
    return ( row << COL_BIT_LEN ) | ( col & ( ( 1 << COL_BIT_LEN ) -1 ));
}
unsigned int bs_torow(int rowcol)
{
    return ( rowcol >> COL_BIT_LEN ) ;
}
unsigned int bs_tocol(int rowcol)
{
    return rowcol & ( ( 1 << COL_BIT_LEN ) -1 );
}

static void addXref(BSParserContext* ctx, BSRowCol loc, int decl)
{
    if( !ctx->xref )
        return;

    const int top = lua_gettop(ctx->L);
    if( decl <= 0 )
        decl += top + 1;

    // #xref table: filepath -> list_of_idents
    // list_of_idents: rowcol -> set_of_decls

    // decl.#xref table: filepath -> set of rowcol

    lua_pushstring(ctx->L, bs_denormalize_path(ctx->filepath) );
    lua_rawget(ctx->L,ctx->xref);
    assert( lua_istable(ctx->L,-1) );
    const int list_of_idents = lua_gettop(ctx->L);

    const unsigned int rowCol = bs_torowcol(loc.row, loc.col);

    lua_pushinteger(ctx->L,rowCol);
    lua_rawget(ctx->L,list_of_idents);
    if( !lua_istable(ctx->L,-1) )
    {
        lua_pop(ctx->L,1); // nil
        lua_createtable(ctx->L,0,0);
        lua_pushinteger(ctx->L,rowCol);
        lua_pushvalue(ctx->L,-2);
        lua_rawset(ctx->L,list_of_idents);
    }
    const int set_of_decls = lua_gettop(ctx->L);
    assert( lua_istable(ctx->L,-1) );

    lua_pushvalue(ctx->L,decl);
    lua_pushboolean(ctx->L,1);
    lua_rawset(ctx->L,set_of_decls);

    lua_pop(ctx->L,2); // list_of_idents, set_of_decls

    lua_getfield(ctx->L, decl, "#xref");
    if( !lua_istable(ctx->L,-1) )
    {
        lua_pop(ctx->L,1); // nil
        lua_createtable(ctx->L,0,0);
        lua_pushvalue(ctx->L,-1);
        lua_setfield(ctx->L,decl, "#xref");
    }
    const int decl_xref = lua_gettop(ctx->L);

    lua_pushstring(ctx->L, bs_denormalize_path(ctx->filepath) );
    lua_rawget(ctx->L,decl_xref);
    if( !lua_istable(ctx->L,-1) )
    {
        lua_pop(ctx->L,1); // nil
        lua_createtable(ctx->L,0,0);
        lua_pushstring(ctx->L, bs_denormalize_path(ctx->filepath) );
        lua_pushvalue(ctx->L,-2);
        lua_rawset(ctx->L,decl_xref);
    }
    const int set_of_rowcol = lua_gettop(ctx->L);

    lua_pushinteger(ctx->L,rowCol);
    lua_pushboolean(ctx->L,1);
    lua_rawset(ctx->L, set_of_rowcol);

    lua_pop(ctx->L,2); // decl_xref, set_of_rowcol
    assert( top == lua_gettop(ctx->L));
}

static void addNumRef(BSParserContext* ctx, int obj)
{
    if( !ctx->numRefs )
        return;
    const int id = lua_objlen(ctx->L,ctx->numRefs) + 1;
    lua_pushvalue(ctx->L,obj);
    lua_rawseti(ctx->L,ctx->numRefs,id);
    lua_pushinteger(ctx->L,id);
    lua_setfield(ctx->L,obj,"#ref");
}

static BSIdentDef identdef(BSParserContext* ctx, BSScope* scope)
{
    BSToken t = nextToken(ctx);
    if( t.tok != Tok_ident )
        error(ctx, t.loc.row, t.loc.col , "expecting an ident");
    BSIdentDef res;
    res.name = t.val;
    res.len = t.len;
    res.loc = t.loc;
    checkUnique(ctx,scope,&res);
    t = peekToken(ctx,1);
    switch( t.tok )
    {
    case Tok_Bang:
        res.visi = BS_PublicDefault;
        nextToken(ctx);
        break;
    case Tok_Star:
        res.visi = BS_Public;
        nextToken(ctx);
        break;
    case Tok_Minus:
        res.visi = BS_Protected;
        nextToken(ctx);
        break;
    default:
        res.visi = BS_Private;
        break;
    }
    return res;
}

static void addToScope(BSParserContext* ctx, BSScope* scope, BSIdentDef* id, int table )
{
    BS_BEGIN_LUA_FUNC(ctx,0);

    lua_pushinteger(ctx->L,id->visi);
    lua_setfield(ctx->L,table,"#visi");

    lua_pushlstring(ctx->L,id->name, id->len);
    lua_setfield(ctx->L,table,"#name");

    lua_pushvalue(ctx->L,scope->table);
    lua_setfield(ctx->L,table,"#owner");

    lua_pushlstring(ctx->L,id->name, id->len);
    lua_pushvalue(ctx->L,table);
    lua_rawset(ctx->L,scope->table);

    lua_pushinteger(ctx->L, ++scope->n );
    lua_pushvalue(ctx->L,table);
    lua_rawset(ctx->L,scope->table);

    BS_END_LUA_FUNC(ctx);
}

static int expression(BSParserContext* ctx, BSScope* scope, int lhsType);
static void parampath(BSParserContext* ctx, const char* name, int len);

static int /*BSPathStatus*/ caclFsDir(BSParserContext* ctx, const char* path, int pathlen)
{
    BS_BEGIN_LUA_FUNC(ctx,1);

    lua_pushvalue(ctx->L, ctx->module.table);
    const int root = lua_gettop(ctx->L);
    lua_getfield(ctx->L,root,"^");
    while( !lua_isnil(ctx->L,-1) )
    {
        lua_replace(ctx->L,root);
        lua_getfield(ctx->L,root,"^");
    }
    lua_pop(ctx->L,1);
    lua_getfield(ctx->L,root,"#dir");
    lua_replace(ctx->L,root);

    if( *path != '/' )
    {
        // this is a relative path, either normalized or not

        // first calc the absolute path of the new subdir (to avoide ../ exhaution)
        lua_getfield(ctx->L,ctx->module.table,"#dir");
        if( *path != '.' )
        {
            // not yet normalized
            lua_pushstring(ctx->L,"./");
            lua_pushlstring(ctx->L,path,pathlen);
            lua_concat(ctx->L,2);
        }else
            lua_pushlstring(ctx->L,path,pathlen);
        // stack: root, #dir, relpath
        int res = bs_add_path(ctx->L, -2, -1);
        if( res != BS_OK )
        {
            lua_pop(ctx->L,3);
            return res;
        }
        // stack: root, #dir, relpath, abspath
        lua_replace(ctx->L,-3);
        lua_pop(ctx->L,1);
    }else
        lua_pushlstring(ctx->L,path,pathlen);

    // stack: root, abs submod fspath

    // now calc the relative path
    int res = bs_makeRelative(lua_tostring(ctx->L,-2),lua_tostring(ctx->L,-1));
    lua_pop(ctx->L,2);
    if( res == BS_OK )
        lua_pushstring(ctx->L, bs_global_buffer() );

    BS_END_LUA_FUNC(ctx);
    return res;
}

static void submodule(BSParserContext* ctx, int subdir)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    nextToken(ctx); // keyword
    BSIdentDef id = identdef(ctx,&ctx->module);
    lua_getfield(ctx->L,ctx->module.table,"#dummy");
    if( !lua_isnil(ctx->L,-1) )
    {
        va_list args;
        va_end(args);
        ctx->logger(BS_Error, ctx->loggerData,ctx->filepath,id.loc, "submod declarations not allowed here", args );
        lua_pushnil(ctx->L);
        lua_error(ctx->L);
    }else
        lua_pop(ctx->L,1);
    const char* path = id.name;
    int pathlen = id.len;
    if( id.visi == BS_PublicDefault )
        error(ctx, id.loc.row, id.loc.col,"'!' is not applicable here" );
    BSToken t = peekToken(ctx,1);
    BSToken pt = t;
    if( t.tok == Tok_Eq || t.tok == Tok_ColonEq )
    {
        nextToken(ctx); // eat '='
        t = nextToken(ctx);
        if( t.tok == Tok_path || t.tok == Tok_ident )
        {
            path = t.val;
            pathlen = t.len;
            pt = t;

            // We now support all paths (no necessity of source-tree and dir-tree correspondence)
            // #rdir is made of idents, not dir names
            // the following code is only for backward compatibility
            if( subdir )
            {
                if( t.tok == Tok_path )
                {
                    // check path is relative and only one level
                    if( *path == '\'' )
                    {
                        path++;
                        pathlen -= 2;
                    }
                    if( strncmp(path,"//",2) == 0 || strncmp(path,"..",2) == 0)
                        error(ctx, pt.loc.row, pt.loc.col,"this path is not supported here" );
                    if( strncmp(path,".",1) == 0 )
                    {
                        path += 2;
                        pathlen -= 2;
                    }
                    int i;
                    for( i = 0; i < pathlen; i++ )
                    {
                        if( path[i] == '/' )
                            error(ctx, pt.loc.row, pt.loc.col,"expecting an immediate subdirectory" );
                    }
                }
            }
        }else
            error(ctx, pt.loc.row, pt.loc.col,"expecting a path or an ident" );
        t = peekToken(ctx,1);
    }

    const char* altpath = 0;
    int altpathlen = 0;
    if( t.tok == Tok_else )
    {
        nextToken(ctx); // eat 'else'
        t = nextToken(ctx);
        if( t.tok == Tok_path )
        {
            altpath = t.val;
            altpathlen = t.len;
            pt = t;
        }else
            error(ctx, pt.loc.row, pt.loc.col,"expecting a path after 'else'" );

        t = peekToken(ctx,1);
    }

    if( t.tok == Tok_Lpar )
    {
        nextToken(ctx); // eat lpar
        t = peekToken(ctx,1);
        while( t.tok != Tok_Rpar && t.tok != Tok_Eof )
        {
            if( t.tok != Tok_ident )
                error(ctx, t.loc.row, t.loc.col,"expexting an identifier" );
            nextToken(ctx); // eat ident
            BSToken pname = t;
            parampath(ctx,id.name,id.len);
            lua_pop(ctx->L,1);
            lua_pushstring(ctx->L,".");
            lua_pushlstring(ctx->L,pname.val,pname.len);
            lua_concat(ctx->L,3);
            const int nm = lua_gettop(ctx->L);
            t = peekToken(ctx,1);
            if( t.tok == Tok_Eq || t.tok == Tok_ColonEq )
            {
                nextToken(ctx); // eat eq
                t = peekToken(ctx,1);
                expression(ctx,&ctx->module,0);
                lua_getfield(ctx->L,-1,"#kind");
                const int kind = lua_tointeger(ctx->L,-1);
                lua_pop(ctx->L,2); // type, kind
                if( kind != BS_BaseType )
                    error(ctx, t.loc.row, t.loc.col,"parameter value must be of basic type" );
                switch( lua_type(ctx->L,-1) )
                {
                case LUA_TBOOLEAN:
                    lua_pushstring(ctx->L, lua_toboolean(ctx->L,-1) ? "true" : "false");
                    lua_replace(ctx->L,-2);
                    break;
                case LUA_TNUMBER:
                    lua_pushstring(ctx->L, lua_tostring(ctx->L,-1));
                    lua_replace(ctx->L,-2);
                    break;
                case LUA_TSTRING:
                    break;
                }
                t = peekToken(ctx,1);
            }else
            {
                lua_pushstring(ctx->L,"true");
            }
            const int val = lua_gettop(ctx->L);
            // check if param is already set, otherwise set it
            lua_pushvalue(ctx->L,nm);
            lua_rawget(ctx->L,BS_Params);
            const int overridden = !lua_isnil(ctx->L,-1);
            lua_pop(ctx->L,1);
            if( overridden )
            {
                warning(ctx,pname.loc.row,pname.loc.col,"parameter %s is overridden by outer value", lua_tostring(ctx->L,nm));
                lua_pop(ctx->L,2); // nm, val
            }else
            {
                lua_createtable(ctx->L,0,0);
                lua_pushvalue(ctx->L,-2);
                lua_rawseti(ctx->L,-2,1); // the param value is a table which carries the value in index 1
                lua_replace(ctx->L,-2);
                // stack: nm, tables
                // because value is boxed in a table the module knows not to do visibility check
                lua_rawset(ctx->L,BS_Params);
            }
            if( t.tok == Tok_Comma )
            {
                nextToken(ctx); // eat comma
                t = peekToken(ctx,1);
            }
        }
        if( t.tok == Tok_Eof )
            error(ctx, t.loc.row, t.loc.col,"non-terminated module parameter list" );
        else
            nextToken(ctx); // eat rpar
    }

    lua_createtable(ctx->L,0,0); // module definition
    const int module = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L, BS_ModuleDef);
    lua_setfield(ctx->L,-2,"#kind");
    lua_pushvalue(ctx->L,ctx->module.table);
    lua_setfield(ctx->L,-2,"^"); // set reference to outer scope
    addToScope(ctx, &ctx->module, &id, module ); // the outer module directly references this module, no adapter
    addXref(ctx, id.loc, module);

    lua_getfield(ctx->L,ctx->module.table,"#rdir");
    lua_pushstring(ctx->L,"/");
    lua_pushlstring(ctx->L,id.name,id.len);
    lua_pushvalue(ctx->L,-1);
    lua_setfield(ctx->L,module,"#dirname");
    lua_concat(ctx->L,3);
    lua_setfield(ctx->L,module,"#rdir"); // construct rdir from root by concatenating the subdir identdefs
    lua_pushinteger(ctx->L,id.loc.row);
    lua_setfield(ctx->L,module,"#row");
    lua_pushinteger(ctx->L,id.loc.col);
    lua_setfield(ctx->L,module,"#col");

    if( altpath )
    {
        if( *altpath == '/' )
            lua_pushlstring(ctx->L,altpath,altpathlen); // this is already an absolute path
        else if( *altpath == '.' )
        {
            // already normalized
            lua_getfield(ctx->L,ctx->module.table,"#dir");
            lua_pushlstring(ctx->L,altpath,altpathlen);
            if( bs_add_path(ctx->L, -2, -1) != 0 )
                error(ctx, pt.loc.row, pt.loc.col,"cannot convert this path (1)" );
            lua_replace(ctx->L,-3);
            lua_pop(ctx->L,1);
        }else
        {
            if(*altpath == '\'')
            {
                altpath++;
                altpathlen -= 2;
            }
            lua_pushstring(ctx->L,ctx->dirpath);
            lua_pushstring(ctx->L,"/");
            lua_pushlstring(ctx->L,altpath,altpathlen);
            lua_concat(ctx->L,3);
        }
        lua_setfield(ctx->L,module,"#altmod");
    }

    lua_pushcfunction(ctx->L, bs_parse);

    if(*path == '\'')
    {
        path++;
        pathlen -= 2;
    }

    if( caclFsDir(ctx, path, pathlen) != BS_OK )
    {
        error(ctx, pt.loc.row, pt.loc.col,"error creating relative file system path" );
    } // else
    lua_setfield(ctx->L,module,"#fsrdir");

    if( *path == '/' )
    {
        lua_pushlstring(ctx->L,path,pathlen); // this is already an absolute path
    }else
    {
        // submod name = relpath
        if( *path == '.' )
        {
            // already normalized, either ./ or ../
            lua_getfield(ctx->L,ctx->module.table,"#dir");
            lua_pushlstring(ctx->L,path,pathlen);
            if( bs_add_path(ctx->L, -2, -1) != 0 )
                error(ctx, pt.loc.row, pt.loc.col,"cannot convert this path (4)" );
            lua_replace(ctx->L,-3);
            lua_pop(ctx->L,1);
        }else
        {
            lua_pushstring(ctx->L,ctx->dirpath);
            lua_pushstring(ctx->L,"/");
            lua_pushlstring(ctx->L,path,pathlen);
            lua_concat(ctx->L,3);
        }
    }
    const int newPath = lua_gettop(ctx->L);

    lua_pushvalue(ctx->L,ctx->module.table);
    while( !lua_isnil(ctx->L,-1) )
    {
        // if path is already present then this module is called recursively, i.e. indefinitely
        lua_getfield(ctx->L,-1,"#dir");
        if( lua_equal(ctx->L,newPath,-1) )
            error(ctx, pt.loc.row, pt.loc.col,"path points to the same directory as current or outer module" );
        lua_pop(ctx->L,1);
        lua_getfield(ctx->L,-1,"^");
        lua_replace(ctx->L,-2);
    }
    lua_pop(ctx->L,1);

    lua_pushvalue(ctx->L,module);

    lua_pushvalue(ctx->L,BS_Params);

    lua_call(ctx->L,3,1); // call bs_parse
    lua_pop(ctx->L,1); // module returned by bs_parse (the same as handed in)

    lua_getfield(ctx->L, ctx->module.table, "#inst"); // the instance of the outer table
    lua_pushlstring(ctx->L,id.name,id.len);
    lua_getfield(ctx->L, module, "#inst"); // the instance of the nested table
    lua_rawset(ctx->L, -3 ); // outer inst points to nested inst by name

    lua_pop(ctx->L,2); // module, outer inst

    BS_END_LUA_FUNC(ctx);
}

static void macrodef(BSParserContext* ctx)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    nextToken(ctx); // keyword
    BSIdentDef id = identdef(ctx,&ctx->module);

    lua_createtable(ctx->L,0,0);
    const int decl = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L,BS_MacroDef);
    lua_setfield(ctx->L,decl,"#kind");
    lua_pushstring(ctx->L,ctx->label);
    lua_setfield(ctx->L,decl,"#source");
    addToScope(ctx, &ctx->module, &id, decl );
    addLocInfo(ctx,id.loc,decl);
    addXref(ctx, id.loc, decl);
    addNumRef(ctx,decl);

    BSToken t = nextToken(ctx);
    if( t.tok == Tok_Lpar )
    {
        // args
        BSToken lpar = t;
        t = nextToken(ctx);
        int n = 0;
        while( t.tok != Tok_Rpar )
        {
            if( t.tok == Tok_Eof )
                error(ctx, lpar.loc.row, lpar.loc.col,"non-terminated argument list" );
            if( t.tok == Tok_ident )
            {
                lua_pushlstring(ctx->L, t.val, t.len);
                const int name = lua_gettop(ctx->L);
                lua_getfield(ctx->L,decl,lua_tostring(ctx->L,name));
                // check for duplicates
                if( !lua_isnil(ctx->L,-1) )
                    error(ctx, t.loc.row, t.loc.col,"duplicate argument name" );
                else
                    lua_pop(ctx->L,1);

                lua_pushvalue(ctx->L,name);
                lua_pushinteger(ctx->L,++n);
                lua_rawset(ctx->L,decl);

                lua_pushvalue(ctx->L,name);
                lua_rawseti(ctx->L,decl,n);

                lua_pop(ctx->L,1); // name
            }else if( t.tok == Tok_Comma )
                ; // ignore
            else
                error(ctx, t.loc.row, t.loc.col,"expecting an identifier or ')'" );

            t = nextToken(ctx);
        }
        t = nextToken(ctx);
    }
    if( t.tok != Tok_Lbrace )
        error(ctx, t.loc.row, t.loc.col,"expecting '{'" );
    BSToken lbrace = t;
    lua_pushinteger(ctx->L,lbrace.loc.row);
    lua_setfield(ctx->L,decl,"#brow");
    lua_pushinteger(ctx->L,lbrace.loc.col);
    lua_setfield(ctx->L,decl,"#bcol");
    int n = 0;
    while(1)
    {
        // TODO: should check syntax already here?
        // i.e. that only { ( subdirectory | declaration | statement ) [';'] } appears
        t = bslex_hnext(ctx->lex);
        if( t.tok == Tok_Lbrace )
            n++;
        else if( t.tok == Tok_Rbrace )
        {
            if( n == 0 )
                break;
            n--;
        }else if( t.tok == Tok_Eof )
            error(ctx, lbrace.loc.row, lbrace.loc.col,"non-terminated macro body" );
        else if( t.tok == Tok_Invalid )
        {
            lua_pushnil(ctx->L);
            lua_error(ctx->L);
        }
    }
    assert( t.tok == Tok_Rbrace );
    lua_pushlstring(ctx->L, lbrace.val, t.val-lbrace.val+1); // include { and } in code fragment
    lua_setfield(ctx->L,decl,"#code");

    lua_pop(ctx->L,1); // decl

    BS_END_LUA_FUNC(ctx);
}

static void goforthis(BSParserContext* ctx, BSScope* scope)
{
    BS_BEGIN_LUA_FUNC(ctx,1);
    lua_pushvalue(ctx->L,scope->table);
    const int curScope = lua_gettop(ctx->L);
    lua_getfield(ctx->L, curScope, "#this" );
    // stack: def, this or nil

    // continue search to surrounding (local) scope (if present)
    while( lua_isnil(ctx->L, -1) )
    {
        lua_pop(ctx->L,1); // remove nil
        // stack: old def
        lua_getfield(ctx->L, curScope, "#up");
        // stack: old def, new def or nil
        if( lua_isnil(ctx->L, -1) )
            break;
        lua_replace(ctx->L,curScope);
        // stack: new def
        lua_getfield(ctx->L, curScope, "#this");
        // stack: new def, this or nil
    }
    // stack: def, this or nil
    lua_replace(ctx->L,curScope);
    BS_END_LUA_FUNC(ctx);
}

// returns 0: read-write, 1: read-only, 2: dot to bound
static int resolveInstance(BSParserContext* ctx, BSScope* scope )
    // out: resolved container instance + derefed declaration
    // NOTE: the order of the declarations is relevant; a decl cannot be dereferenced before it appears in the text
{
    BS_BEGIN_LUA_FUNC(ctx,2);
    BSToken t = nextToken(ctx);
    int ret = 0;
    enum { LocalOnly, LocalOuter, Field };
    int method;
    switch( t.tok )
    {
    case Tok_Dot:
        method = Field;
        //lua_getfield(ctx->L, scope->table, "#this" );
        goforthis(ctx,scope);

        if( lua_isnil(ctx->L,-1) )
            error(ctx, t.loc.row, t.loc.col,"designator cannot start with '.' here" );
        // from here we have the instance on the stack
        ret = 2;
        t = nextToken(ctx);
        break;
    case Tok_Hat:
        method = LocalOuter;
        lua_pushvalue(ctx->L,ctx->module.table); // use module instead of scope because outer is only available on mod level
        lua_getfield(ctx->L, -1, "^");
        lua_remove(ctx->L,-2);
        // from here we have the nearest outer module definition on the stack, or nil
        t = nextToken(ctx);
        break;
    case Tok_ident:
        method = LocalOnly;
        lua_getfield(ctx->L,scope->table,"#inst");
        // from here we have the module or block instance on the stack
        break;
    default:
        error(ctx, t.loc.row, t.loc.col, "designator must start with a '^', '.' or identifier" );
        break;
    }
    // here we have the first scope on stack from where we resolve the ident
    if( t.tok != Tok_ident )
        error(ctx, t.loc.row, t.loc.col, "expecting an identifier here" );

    // now resolve the first ident of the desig
    if( method == LocalOuter )
    {
        // start with outer module decl on stack
        while( !lua_isnil(ctx->L,-1) )
        {
            lua_pushlstring(ctx->L, t.val, t.len);
            lua_rawget(ctx->L,-2);
            if( !lua_isnil(ctx->L, -1) )
            {
                // we have a hit;
                // stack: module def, decl
                lua_getfield(ctx->L,-1,"#visi");
                const int visi = lua_tointeger(ctx->L,-1);
                lua_pop(ctx->L,1);
                if( visi == BS_Private )
                    error(ctx, t.loc.row, t.loc.col, "the identifier is not visible from here" );
                else
                {
                    // stack: module def, decl
                    lua_getfield(ctx->L,-2,"#inst");
                    // stack: module def, decl, inst
                    lua_replace(ctx->L,-3); // replace the module def by its instance
                    // result stack: module inst, decl
                    addXref(ctx, t.loc, -1);
                    break;
                }
            }else
            {
                // continue search to outer module (if present)
                lua_pop(ctx->L,1); // remove nil
                lua_getfield(ctx->L, -1, "^");
                lua_remove(ctx->L,-2);
            }
        }
    }else
    {
        // stack: module, body or object instance
        // method == LocalOnly or Field
        lua_getmetatable(ctx->L,-1);
        // stack: instance, def
        lua_pushlstring(ctx->L, t.val, t.len);
        lua_rawget(ctx->L,-2);
        lua_remove(ctx->L,-2);
        // stack: instance, derefed decl or nil
        if( !lua_isnil(ctx->L, -1) )
        {
            // we have a hit
            // stack: container instance, derefed decl
            addXref(ctx, t.loc, -1);
        }else if( method != Field )
        {
            // continue search to surrounding (local) scope (if present)
            while( lua_isnil(ctx->L, -1) )
            {
                lua_pop(ctx->L,1); // remove nil
                // stack: old instance
                lua_getmetatable(ctx->L,-1);
                // stack: old instance, old def
                lua_getfield(ctx->L, -1, "#up");
                // stack: old instance, old def, new def or nil
                lua_replace(ctx->L,-2);
                // stack: old instance, new def or nil
                if( lua_isnil(ctx->L, -1) )
                    break;
                lua_getfield(ctx->L, -1, "#inst");
                // stack: old instance, new def, new inst
                lua_replace(ctx->L,-3);
                // stack: new instance, new def
                lua_pushlstring(ctx->L, t.val, t.len);
                lua_rawget(ctx->L,-2);
                // stack: new instance, new def, derefed decl or nil
                lua_remove(ctx->L,-2);
                // stack: new instance, derefed decl or nil
            }

            if( lua_isnil(ctx->L, -1) )
            {
                // no hit, directly look in builtins (we don't need to prefix them with ^ to desig them)
                lua_pop(ctx->L,2); // instance, nil
                lua_getfield(ctx->L,ctx->builtins,"#inst");
                lua_pushlstring(ctx->L, t.val, t.len);
                lua_rawget(ctx->L,ctx->builtins);
                if( !lua_isnil(ctx->L, -1) )
                {
                    // we have a hit
                    // stack: container instance (not builtins table!), derefed builtin decl
                    addXref(ctx, t.loc, -1);
               }
            }else
                addXref(ctx, t.loc, -1); // we have a hit
        }
    }
    if( lua_isnil(ctx->L,-1) )
        error(ctx, t.loc.row, t.loc.col,
               "identifier doesn't reference a declaration; check spelling and declaration order" );
    if( method != Field )
    {
        lua_getfield(ctx->L,-1,"#rw");
        const int rw = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        if( rw == BS_let || ( method != LocalOnly && rw == BS_param ) )
            ret = 1;
    }

    // Now we have top on stack an instance of BS_ModuleDef, BS_BlockDef or BS_ClassDecl and a
    //   dereferenced declaration,
    // i.e. a BS_ClassDecl, BS_EnumDecl, BS_VarDecl, BS_FieldDecl, BS_Proc or BS_ModuleDef (from subdir)

    int line = t.loc.row;
    t = peekToken(ctx,1);
    while( t.tok == Tok_Dot )
    {
        // stack: container instance, derefed decl
        // the derefed decl name is a value in the container
        t = nextToken(ctx); // eat dot
        // fetch new instance
        lua_getfield(ctx->L,-1,"#name");
        lua_rawget(ctx->L,-3);
        // stack: container instance, derefed decl, new instance
        // the new instance is the resolved value from the container

        lua_getfield(ctx->L,-2,"#kind");
        int kind = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);

        switch( kind ) // kind of derefed decl
        {
        case BS_BaseType:
        case BS_ListType:
        case BS_ClassDecl:
        case BS_EnumDecl:
        case BS_ProcDef:
            error(ctx, t.loc.row, t.loc.col, "cannot dereference a type declaration or procedure" );
            break;
        case BS_FieldDecl:
        case BS_VarDecl:
            // we want to dereference a var or field, so we need the class type of it
            // stack: container instance, derefed decl, new instance
            lua_getfield(ctx->L,-2,"#type");
            // stack: old instance, derefed decl, new instance, class decl

            lua_getfield(ctx->L,-1,"#kind");
            if( lua_tointeger(ctx->L,-1) != BS_ClassDecl )
            {
                if( t.loc.row != line )
                    warning(ctx,t.loc.row,t.loc.col,"designator has wrapped around from the previous line; did you miss a semicolon?");
                error(ctx, t.loc.row, t.loc.col, "can only dereference fields or variables of class type" );
            }
            lua_pop(ctx->L,1); // kind

            lua_replace(ctx->L,-3); // remove the field or var decl; top is now a classdecl
            // stack: old instance, classdecl, new instance
            break;
        case BS_ModuleDef:
            break;
        default:
            assert(0);
        }

        // stack: old instance, classdecl or module decl (in case of subdir), new instance
        if( lua_isnil(ctx->L,-3) )
            error(ctx, t.loc.row, t.loc.col, "dereferencing a nil value" );

        t = nextToken(ctx); // ident
        if( t.tok != Tok_ident )
            error(ctx, t.loc.row, t.loc.col, "expecting an ident" );

        if( t.loc.row != line )
        {
            warning(ctx,t.loc.row,t.loc.col,"designator wraps around the next line; did you miss a semicolon?");
            line = t.loc.row;
        }
        lua_pushlstring(ctx->L, t.val, t.len);
        lua_rawget(ctx->L,-3);
        // stack: old instance, class/mod decl, new instance, derefed decl or nil
        lua_replace(ctx->L,-3);
        // stack: old instance, derefed decl or nil, new instance

        if( lua_isnil(ctx->L,-2) )
            error(ctx, t.loc.row, t.loc.col, "unknown identifier" );

        lua_replace(ctx->L, -3);
        // stack: new instance, derefed decl
        addXref(ctx, t.loc, -1);

        lua_getfield(ctx->L,-1,"#kind");
        kind = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);

        switch( kind )
        {
        case BS_ModuleDef:
        case BS_VarDecl:
            lua_getfield(ctx->L,-1,"#visi");
            if( lua_tointeger(ctx->L,-1) < BS_Public )
                error(ctx, t.loc.row, t.loc.col, "the identifier is not visible from here" );
            lua_pop(ctx->L,1);
            break;
        }
        // here BS_ModuleDef, BS_VarDecl and BS_FieldDecl are left

        t = peekToken(ctx,1);
    }
    BS_END_LUA_FUNC(ctx);
    return ret;
}

// this procedure resolves types, not instances
static void resolveDecl(BSParserContext* ctx, BSScope* scope )
    // out: resolved symbol is on the stack
{
    BS_BEGIN_LUA_FUNC(ctx,1);

    resolveInstance(ctx,scope);
    lua_remove(ctx->L,-2);

    BS_END_LUA_FUNC(ctx);
}

static void enumdecl(BSParserContext* ctx, BSScope* scope, BSIdentDef* id)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    BSToken t = nextToken(ctx);
    BSToken lpar = t;
    lua_createtable(ctx->L,0,0);
    const int decl = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L,BS_EnumDecl);
    lua_setfield(ctx->L,decl,"#kind");
    addToScope(ctx, scope, id, decl );
    addLocInfo(ctx,id->loc,decl);
    addXref(ctx, id->loc, decl);
    addNumRef(ctx,decl);

    t = nextToken(ctx);
    int n = 0;
    while( t.tok != Tok_Rpar )
    {
        if( t.tok == Tok_Eof )
            error(ctx, lpar.loc.row, lpar.loc.col,"non-terminated enum type declaration" );
        if( t.tok == Tok_symbol )
        {
            lua_pushlstring(ctx->L, t.val+1, t.len-1); // remove leading `
            const int name = lua_gettop(ctx->L);
            lua_getfield(ctx->L,decl,lua_tostring(ctx->L,name));
            // check for duplicates
            if( !lua_isnil(ctx->L,-1) )
                error(ctx, t.loc.row, t.loc.col,"duplicate field name" );
            else
                lua_pop(ctx->L,1);

            if( n == 0)
            {
                // the first symbol is the default value
                lua_pushvalue(ctx->L,name);
                lua_setfield(ctx->L,decl,"#default");
            }
            lua_pushvalue(ctx->L,name);
            lua_pushinteger(ctx->L,++n);
            lua_rawset(ctx->L,decl);

            lua_pop(ctx->L,1); // name
        }else if( t.tok == Tok_Comma )
            ; // ignore
        else
            error(ctx, t.loc.row, t.loc.col,"expecting a symbol or ')'" );

        t = nextToken(ctx);
    }
    if( n == 0 )
        error(ctx, t.loc.row, t.loc.col,"enum type cannot be empty" );

    lua_pop(ctx->L,1); // decl
    BS_END_LUA_FUNC(ctx);
}

static void typeref(BSParserContext* ctx, BSScope* scope)
{
    BS_BEGIN_LUA_FUNC(ctx,1);
    BSToken t = peekToken(ctx,1);
    resolveDecl(ctx,scope);
    const int type = lua_gettop(ctx->L);

    // check whether really a type
    lua_getfield(ctx->L,type,"#kind");
    const int kind = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    if( kind != BS_BaseType && kind != BS_ClassDecl && kind != BS_EnumDecl ) // we don't support lists of lists
        error(ctx, t.loc.row, t.loc.col,"designator doesn't point to a valid type" );

    t = peekToken(ctx,1);
    if( t.tok == Tok_LbrackRbrack )
    {
        nextToken(ctx); // eat it
        lua_createtable(ctx->L,0,0);
        const int ptr = lua_gettop(ctx->L);
        lua_pushinteger(ctx->L,BS_ListType);
        lua_setfield(ctx->L,ptr,"#kind");
        // set pointer type
        lua_pushvalue(ctx->L,type);
        lua_setfield(ctx->L,ptr,"#type");

        lua_remove(ctx->L, type);
    }
    BS_END_LUA_FUNC(ctx);
}

static int endOfBlock( BSToken* t, int pascal )
{
    if( pascal )
        return t->tok == Tok_end || t->tok == Tok_elsif || t->tok == Tok_else;
    else
        return t->tok == Tok_Rbrace;
}

static void classdecl(BSParserContext* ctx, BSScope* scope, BSIdentDef* id)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    BSToken t = nextToken(ctx);
    BSToken cls = t;
    lua_createtable(ctx->L,0,0);
    const int clsDecl = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L,BS_ClassDecl);
    lua_setfield(ctx->L,clsDecl,"#kind");
    addLocInfo(ctx,id->loc,clsDecl);
    addXref(ctx, id->loc, clsDecl);
    addNumRef(ctx,clsDecl);

    t = peekToken(ctx,1);
    int n = 0;
    if( t.tok == Tok_Lpar )
    {
        nextToken(ctx); // eat it
        resolveDecl(ctx, scope);
        assert( !lua_isnil(ctx->L,-1) );
        lua_getfield(ctx->L,-1,"#kind");
        if( lua_tointeger(ctx->L,-1) != BS_ClassDecl )
            error(ctx, t.loc.row, t.loc.col,"invalid superclass" );
        lua_pop(ctx->L,1); // kind
        const int super = lua_gettop(ctx->L);

        // NOTE: circular superclass chaines are not possible because addToScope was not yet called
        lua_pushvalue(ctx->L,super);
        lua_setfield(ctx->L,clsDecl,"#super");

        n = lua_objlen(ctx->L,super);
        int i;
        for( i = 1; i <= n; i++ )
        {
            // copy down all inherited fields (by name and by number)
            lua_rawgeti(ctx->L,super,i);
            lua_getfield(ctx->L,-1,"#name");
            lua_pushvalue(ctx->L, -2 );
            lua_rawset(ctx->L,clsDecl);
            lua_rawseti(ctx->L,clsDecl,i);
        }

        t = nextToken(ctx);
        if( t.tok != Tok_Rpar )
            error(ctx, t.loc.row, t.loc.col ,"expecting ')'");
        t = peekToken(ctx,1);

        lua_pop(ctx->L,1); // super
    }
    const int pascal = t.tok != Tok_Lbrace;
    if( !pascal )
        t = nextToken(ctx); // eat {
        // no longer useful because of pascal version: error2(ctx, t.loc.row, t.loc.col,"expecting '{'" );

    addToScope(ctx, scope, id, clsDecl );

    t = nextToken(ctx);
    while( !endOfBlock(&t,pascal) )
    {
        if( t.tok == Tok_Eof )
            error(ctx, cls.loc.row, cls.loc.col,"non-terminated class declaration" );
        if( t.tok != Tok_ident )
            error(ctx, t.loc.row, t.loc.col,"expecting identifier" );

        lua_pushlstring(ctx->L,t.val,t.len);
        const int name = lua_gettop(ctx->L);

        lua_getfield(ctx->L,clsDecl,lua_tostring(ctx->L,name));
        // check for duplicates
        if( !lua_isnil(ctx->L,-1) )
            error(ctx, t.loc.row, t.loc.col,"duplicate field name" );
        else
            lua_pop(ctx->L,1);

        lua_createtable(ctx->L,0,0);
        const int field = lua_gettop(ctx->L);
        // the following does the same as addToScope, but for the class
        lua_pushinteger(ctx->L,BS_FieldDecl);
        lua_setfield(ctx->L,field,"#kind");
        addLocInfo(ctx,t.loc,field);

        lua_pushvalue(ctx->L,name);
        lua_setfield(ctx->L,field,"#name");

        lua_pushvalue(ctx->L,clsDecl);
        lua_setfield(ctx->L,field,"#owner"); // field points to class

        lua_pushvalue(ctx->L,name);
        lua_pushvalue(ctx->L,field);
        lua_rawset(ctx->L,clsDecl); // class points to field by name

        lua_pushinteger(ctx->L,++n);
        lua_pushvalue(ctx->L,field);
        lua_rawset(ctx->L,clsDecl); // class points to field by ordinal number

        t = nextToken(ctx);
        if( t.tok != Tok_Colon )
            error(ctx, t.loc.row, t.loc.col,"expecting ':'" );

        t = peekToken(ctx,1);
        typeref(ctx,scope);

        lua_getfield(ctx->L,-1,"#kind");
        const int kind = lua_tointeger(ctx->L, -1);
        lua_pop(ctx->L,1);
        if( kind == BS_ClassDecl )
            error(ctx, t.loc.row, t.loc.col,"fields cannot be of class type; use a list instead" );
            // otherwise we have no default initializer; the default initializer of enum is its first item

        lua_setfield(ctx->L,field,"#type"); // field points to its type

        lua_pop(ctx->L,2); // name, field
        t = nextToken(ctx);
        if( t.tok == Tok_Semi )
            t = nextToken(ctx);
    }

    lua_pop(ctx->L,1); // decl
    BS_END_LUA_FUNC(ctx);
}

static void typedecl(BSParserContext* ctx, BSScope* scope)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    nextToken(ctx); // keyword
    BSIdentDef id = identdef(ctx, scope);
    if( id.visi == BS_PublicDefault )
        error(ctx, id.loc.row, id.loc.col,"'!' is not applicable here" );
    BSToken t = nextToken(ctx);
    if( t.tok != Tok_Eq )
        error(ctx, t.loc.row, t.loc.col,"expecting '='" );
    t = peekToken(ctx,1);
    switch( t.tok )
    {
    case Tok_Lpar:
        enumdecl(ctx,scope,&id);
        break;
    case Tok_class:
        classdecl(ctx,scope,&id);
        break;
    default:
        error(ctx, t.loc.row, t.loc.col,"invalid type declaration" );
    }

    BS_END_LUA_FUNC(ctx);
}

static int isInEnum( BSParserContext* ctx, int type, int sym )
{
    const int top = lua_gettop(ctx->L);
    if( type <= 0 )
        type += top + 1;
    if( sym <= 0 )
        sym += top + 1;
    if( !lua_istable(ctx->L,type) )
        return 0;
    lua_getfield(ctx->L,type,"#kind");
    const int k = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    if( k != BS_EnumDecl )
        return 0;
    lua_pushvalue(ctx->L,sym);
    lua_rawget(ctx->L,type);
    const int found = !lua_isnil(ctx->L,-1);
    lua_pop(ctx->L,1);

    return found;
}

static int sameType( BSParserContext* ctx, int left, int right )
{
    const int top = lua_gettop(ctx->L);
    if( left <= 0 )
        left += top + 1;
    if( right <= 0 )
        right += top + 1;
    if( lua_equal(ctx->L,left,right) )
        return 1;
    if( !lua_istable(ctx->L,left) || !lua_istable(ctx->L,right) )
        return 0;
    lua_getfield(ctx->L,left,"#kind");
    const int kleft = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    lua_getfield(ctx->L,right,"#kind");
    const int kright = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
#if 0 // doesnt work since right is a type, not a value
    if( kleft == BS_EnumDecl && kright == BS_BaseType )
        return isInEnum(ctx,left,right);
    if( kright == BS_EnumDecl && kleft == BS_BaseType )
        return isInEnum(ctx,right,left);
#endif
    if( kleft != kright )
        return 0;
    if( kleft == BS_ClassDecl || kright == BS_ClassDecl || kleft == BS_EnumDecl || kright == BS_EnumDecl )
        return 0; // we already checked whether table a and b ar the same
    if( kleft == BS_BaseType && kright == BS_BaseType )
    {
        lua_getfield(ctx->L,left,"#type");
        const int tl = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        lua_getfield(ctx->L,right,"#type");
        const int tr = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        return tl == tr;
    }
    if( kleft == BS_ListType && kright == BS_ListType )
    {
        lua_getfield(ctx->L,left,"#type");
        lua_getfield(ctx->L,right,"#type");
        const int res = sameType(ctx,-2,-1);
        lua_pop(ctx->L,2);
        return res;
    }
    return 0;
}

int bs_isa( lua_State *L, int lhs, int rhs )
{
    const int top = lua_gettop(L);
    if( lhs <= 0 )
        lhs += top + 1;
    if( rhs <= 0 )
        rhs += top + 1;
    if( !lua_istable(L,lhs) || !lua_istable(L,rhs) )
        return 0;
    lua_getfield(L,lhs,"#kind");
    const int ka = lua_tointeger(L,-1);
    lua_pop(L,1);
    lua_getfield(L,rhs,"#kind");
    const int kb = lua_tointeger(L,-1);
    lua_pop(L,1);
    if( ka != BS_ClassDecl && kb != BS_ClassDecl )
        return 0;

    lua_pushvalue(L,rhs);
    const int type = lua_gettop(L);
    while( !lua_isnil(L,type) )
    {
        if( lua_equal(L,lhs,type) )
        {
            lua_pop(L,1); // type
            return 1;
        }
        lua_getfield(L,type,"#super");
        lua_replace(L,type);
    }
    lua_pop(L,1); // type
    assert( top == lua_gettop(L));
    return 0;
}

static int isSameOrSubclass( BSParserContext* ctx, int lhs, int rhs )
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    const int res = bs_isa(ctx->L,lhs,rhs);
    BS_END_LUA_FUNC(ctx);
    return res;
}

static void samelist(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 2 )
        error(ctx, row, col,"expecting two arguments" );
    const int arg1 = lua_gettop(ctx->L) - 4 + 1;
    const int arg2 = arg1 + 2;
    lua_getfield(ctx->L,arg1+1,"#kind");
    lua_getfield(ctx->L,arg2+1,"#kind");
    if( lua_tointeger(ctx->L,-1) != BS_ListType || lua_tointeger(ctx->L,-2) != BS_ListType )
        error(ctx, row, col,"expecting two arguments of list type");
    lua_pop(ctx->L,2);
    lua_getfield(ctx->L,arg1+1,"#type");
    lua_getfield(ctx->L,arg2+1,"#type");
    if( !sameType(ctx,-2,-1) )
        error(ctx, row, col,"expecting two arguments of same list type" );
    lua_pop(ctx->L,2);

    const int nl = lua_objlen(ctx->L,arg1);
    const int nr = lua_objlen(ctx->L,arg2);
    int eq = (nl == nr);
    if( eq )
    {
        int i;
        for( i = 1; i <= nl; i++ )
        {
            lua_rawgeti(ctx->L,arg1,i);
            lua_rawgeti(ctx->L,arg2,i);
            eq = lua_equal(ctx->L,-1,-2);
            lua_pop(ctx->L,2);
            if( !eq )
                break;
        }
    }

    lua_pushboolean(ctx->L, eq );
    lua_getfield(ctx->L,ctx->builtins,"bool");
    BS_END_LUA_FUNC(ctx);
}

static void sameset(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 2 )
        error(ctx, row, col,"expecting two arguments" );
    const int arg1 = lua_gettop(ctx->L) - 4 + 1;
    const int arg2 = arg1 + 2;
    lua_getfield(ctx->L,arg1+1,"#kind");
    lua_getfield(ctx->L,arg2+1,"#kind");
    if( lua_tointeger(ctx->L,-1) != BS_ListType || lua_tointeger(ctx->L,-2) != BS_ListType )
        error(ctx, row, col,"expecting two arguments of list type" );
    lua_pop(ctx->L,2);
    lua_getfield(ctx->L,arg1+1,"#type");
    lua_getfield(ctx->L,arg2+1,"#type");
    if( !sameType(ctx,-2,-1) )
        error(ctx, row, col,"expecting two arguments of same list type" );
    lua_pop(ctx->L,2);

    const int nl = lua_objlen(ctx->L,arg1);
    const int nr = lua_objlen(ctx->L,arg2);
    lua_createtable(ctx->L,nr,0);
    const int tmp = lua_gettop(ctx->L);
    int i;
    for( i = 1; i <= nr; i++ )
    {
        lua_rawgeti(ctx->L,arg2,i);
        lua_pushinteger(ctx->L,i);
        lua_rawset(ctx->L,tmp);
    }
    int eq = 1;
    for( i = 1; i <= nl; i++ )
    {
        lua_rawgeti(ctx->L,arg1,i);
        lua_rawget(ctx->L,tmp);
        if( lua_isnil(ctx->L,-1) )
            eq = 0;
        lua_pop(ctx->L,1);
        if( !eq )
            break;
    }
    lua_pop(ctx->L,1); // tmp

    lua_pushboolean(ctx->L, eq );
    lua_getfield(ctx->L,ctx->builtins,"bool");
    BS_END_LUA_FUNC(ctx);
}

static void abspath(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n == 0 )
    {
        lua_getfield(ctx->L,ctx->module.table,"#dir");
        lua_getfield(ctx->L,ctx->builtins, "path");
    }else if( n == 1 )
    {
        if( lua_isnil(ctx->L,-1) && lua_istable(ctx->L,-2) )
        {
            lua_getmetatable(ctx->L,-2);
            lua_getfield(ctx->L,-1,"#kind");
            if( lua_tointeger(ctx->L,-1) != BS_ModuleDef )
                error(ctx, row, col,"invalid argument type" );
            lua_pop(ctx->L,1);
            lua_getfield(ctx->L,-1,"#dir");
            lua_replace(ctx->L,-2);
        }else
        {
            lua_getfield(ctx->L,-1,"#kind");
            lua_getfield(ctx->L,-2,"#type");
            if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_path )
                error(ctx, row, col,"expecting argument of type path" );
            lua_pop(ctx->L,2);
            if( *lua_tostring(ctx->L,-2) == '/' )
                lua_pushvalue(ctx->L,-2);
            else
            {
                lua_getfield(ctx->L,ctx->module.table,"#dir");
                if( bs_add_path(ctx->L,-1,-3) != 0 )
                    error(ctx, row, col,"cannot convert this path (5)" );
            }
            lua_replace(ctx->L,-2);
        }
        lua_getfield(ctx->L,ctx->builtins, "path");
    }else if( n == 2 )
    {
        lua_getfield(ctx->L,-1,"#kind");
        lua_getfield(ctx->L,-2,"#type");
        if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_path )
            error(ctx, row, col,"expecting second argument of type path" );
        lua_pop(ctx->L,2);
        if( !lua_isnil(ctx->L,-3) || !lua_istable(ctx->L,-4) )
            error(ctx, row, col,"expecting first argument of module type" );
        if( *lua_tostring(ctx->L,-2) == '/' )
            lua_pushvalue(ctx->L,-2);
            // stack: v1, m, v2, t2, v2
        else
        {
            lua_getmetatable(ctx->L,-4);
            // stack: v1, t1, v2, t2, m
            lua_getfield(ctx->L,-1,"#dir");
            // v1, t1, v2, t2, m, dir
            lua_replace(ctx->L,-2);
            // v1, t1, v2, t2, dir
            if( bs_add_path(ctx->L,-1,-3) != 0 )
                error(ctx, row, col,"cannot convert this path (6)" );
            // v1, t1, v2, t2, dir, path
            lua_replace(ctx->L,-2);
            // v1, t1, v2, t2, path
        }
        lua_getfield(ctx->L,ctx->builtins, "path");
        // dir, t
    }else
        error(ctx, row, col,"expecting zero, one or two arguments" );
    BS_END_LUA_FUNC(ctx);
}

static void readstring(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_path )
        error(ctx, row, col,"expecting one argument of type path" );
    lua_pop(ctx->L,2);

    // stack: value, type
    if( *lua_tostring(ctx->L,-2) != '/' )
    {
        lua_getfield(ctx->L,ctx->module.table,"#dir");
        if( bs_add_path(ctx->L,-1,-3) != 0 )
            error(ctx, row, col,"cannot convert this path (7)" );
        // stack: rel, type, field, abs
        lua_replace(ctx->L,-4);
        lua_pop(ctx->L,1);
    }

    if( !ctx->skipMode )
    {
        FILE* f = bs_fopen(bs_denormalize_path(lua_tostring(ctx->L,-2)),"r");
        if( f == NULL )
            error(ctx, row, col,"cannot open file for reading: %s", lua_tostring(ctx->L,-2) );
        fseek(f, 0L, SEEK_END);
        int sz = ftell(f);
        if( sz < 0 )
            error(ctx, row, col,"cannot determine file size: %s", lua_tostring(ctx->L,-2) );
        rewind(f);
        if( sz > 16000 )
            error(ctx, row, col,"file is too big to be read: %s", lua_tostring(ctx->L,-2) );
        char* tmp1 = (char*) malloc(sz+1);
        char* tmp2 = (char*) malloc(2*sz+1);
        if( tmp1 == NULL || tmp2 == NULL )
            error(ctx, row, col,"not enough memory to read file: %s", lua_tostring(ctx->L,-2) );
        if( fread(tmp1,1,sz,f) != (size_t)sz )
        {
            free(tmp1);
            free(tmp2);
            error(ctx, row, col,"error reading file: %s", lua_tostring(ctx->L,-2) );
        }
        tmp1[sz] = 0;
        char* p = tmp1;
        char* q = tmp2;
        char* lastnws = 0;
        while( sz )
        {
            uchar n = 0;
            const uint ch = unicode_decode_utf8((const uchar*) p, &n);
            if( n == 0 || ch == 0 )
            {
                free(tmp1);
                free(tmp2);
                error(ctx, row, col,"invalid utf-8 format: %s", lua_tostring(ctx->L,-2) );
            }
            if( unicode_isspace(ch) && q == tmp2 )
                ; // swallow leading white space
            else
                switch(ch)
                {
                case '\n':
                case '\r':
                case '\b':
                case '\f':
                case '\t':
                case '\v':
                    *q++ = ' ';
                    break;
                case '\\':
                    *q++ = '\\';
                    *q = '\\';
                    lastnws = q++;
                    break;
                case '"':
                    *q++ = '\\';
                    *q = '"';
                    lastnws = q++;
                    break;
                default:
                    if( !unicode_isspace(ch) )
                        lastnws = q;
                    memcpy(q,p,n);
                    q += n;
                }
            sz -= n;
            p += n;
        }
        if( lastnws )
            q = lastnws + 1;
        *q = 0;
        lua_pushstring(ctx->L, tmp2);
        free(tmp1);
        free(tmp2);
    }else
        lua_pushstring(ctx->L,"");
    lua_getfield(ctx->L,ctx->builtins, "string");
    BS_END_LUA_FUNC(ctx);
}

static void relpath(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n == 0 )
    {
        lua_getfield(ctx->L,ctx->module.table,"#rdir");
        lua_getfield(ctx->L,ctx->builtins, "path");
    }else if( n == 1 )
    {
        if( lua_isnil(ctx->L,-1) && lua_istable(ctx->L,-2) )
        {
            lua_getmetatable(ctx->L,-2);
            lua_getfield(ctx->L,-1,"#kind");
            if( lua_tointeger(ctx->L,-1) != BS_ModuleDef )
                error(ctx, row, col,"invalid argument type" );
            lua_pop(ctx->L,1);
            lua_getfield(ctx->L,-1,"#rdir");
            lua_replace(ctx->L,-2);
            lua_getfield(ctx->L,ctx->builtins, "path");
        }else
            error(ctx, row, col,"invalid argument type" );
    }else
        error(ctx, row, col,"expecting zero or one arguments" );
    BS_END_LUA_FUNC(ctx);
}

static void modname(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n == 0 )
    {
        lua_getfield(ctx->L,ctx->module.table,"#label");
        lua_getfield(ctx->L,ctx->builtins, "string");
    }else if( n == 1 )
    {
        if( lua_isnil(ctx->L,-1) && lua_istable(ctx->L,-2) )
        {
            lua_getmetatable(ctx->L,-2);
            lua_getfield(ctx->L,-1,"#kind");
            if( lua_tointeger(ctx->L,-1) != BS_ModuleDef )
                error(ctx, row, col,"invalid argument type" );
            lua_pop(ctx->L,1);
            lua_getfield(ctx->L,-1,"#label");
            lua_replace(ctx->L,-2);
            lua_getfield(ctx->L,ctx->builtins, "string");
        }else
            error(ctx, row, col,"invalid argument type" );
    }else
        error(ctx, row, col,"expecting zero or one arguments" );
    BS_END_LUA_FUNC(ctx);
}

static void build_dir(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 0 )
        error(ctx, row, col,"expecting zero arguments" );

    lua_getfield(ctx->L,ctx->builtins,"#inst");
    lua_getfield(ctx->L,-1,"root_build_dir");
    lua_replace(ctx->L,-2);
    lua_getfield(ctx->L,ctx->module.table,"#rdir");
    bs_add_path(ctx->L, -2, -1);
    lua_replace(ctx->L,-3);
    lua_pop(ctx->L,1);
    lua_getfield(ctx->L,ctx->builtins, "path");

    BS_END_LUA_FUNC(ctx);
}

static void toint(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_real )
        error(ctx, row, col,"expecting one argument of type real" );
    lua_pop(ctx->L,2);
    lua_pushinteger(ctx->L, lua_tonumber(ctx->L,-2));
    lua_getfield(ctx->L,ctx->builtins, "int");
    BS_END_LUA_FUNC(ctx);
}

static void toreal(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_integer )
        error(ctx, row, col,"expecting one argument of type integer" );
    lua_pop(ctx->L,2);
    lua_pushnumber(ctx->L, lua_tointeger(ctx->L,-2));
    lua_getfield(ctx->L,ctx->builtins, "real");
    BS_END_LUA_FUNC(ctx);
}

static void tostring(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    const int k = lua_tointeger(ctx->L,-2);
    if( k != BS_BaseType && k != BS_EnumDecl )
        error(ctx, row, col,"expecting one argument of a base type" );
    const int type = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,2);
    switch(type)
    {
    case BS_boolean:
        lua_pushstring(ctx->L, lua_toboolean(ctx->L,-2) ? "true" : "false" );
        break;
    case BS_path:
        {
#if 0
            lua_getfield(ctx->L,ctx->builtins,"#inst");
            lua_getfield(ctx->L,-1,"host_os");
            const int iswin32 = strcmp(lua_tostring(ctx->L,-1),"win32") == 0;
            lua_pop(ctx->L,2);
#endif
            lua_pushstring(ctx->L, bs_denormalize_path(lua_tostring(ctx->L,-2)) );
#if 0
            // not necessary; win32 can pretty well do with forward instead of backslashes
            if( iswin32 )
            {
                char* str = (char*)lua_tostring(ctx->L,-1);
                char* p = str;
                while(*p != 0)
                {
                    if(*p == '/')
                        *p = '\\';
                    p++;
                }
                lua_pushstring(ctx->L,str);
                lua_replace(ctx->L,-2);
            }
#endif
        }
        break;
    default:
        lua_pushstring(ctx->L, lua_tostring(ctx->L,-2) );
        break;
    }
    lua_getfield(ctx->L,ctx->builtins, "string");
    BS_END_LUA_FUNC(ctx);
}

static void topath(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_string )
        error(ctx, row, col,"expecting one argument of string type" );
    lua_pop(ctx->L,2);
    const char* str = lua_tostring(ctx->L,-2);

    BSPathStatus res = bs_normalize_path2(str);
    switch(res)
    {
    case BS_OK:
        break;
    case BS_NotSupported:
        error(ctx, row, col,"this path format is not supported" );
        break;
    case BS_InvalidFormat:
        error(ctx, row, col,"this path format is invalid" );
        break;
    case BS_OutOfSpace:
        error(ctx, row, col,"this path is too long to be handled" );
        break;
    case BS_NOP:
        assert(0);
        break; // never happens
    }

    lua_pushstring(ctx->L,bs_global_buffer());
    lua_getfield(ctx->L,ctx->builtins, "path");
    BS_END_LUA_FUNC(ctx);
}

static inline const char* labelOrFilepath(BSParserContext* ctx)
{
    if( ctx->logger == bs_defaultLogger )
        return ctx->label;
    else
        return bs_denormalize_path(ctx->filepath);
}

// kind = 0: message; = 1: warning; = 2: error
static void print(BSParserContext* ctx, int n, int row, int col, int kind)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n < 1 )
        error(ctx, row, col,"expecting at least one argument" );
    int i;
    const int first = lua_gettop(ctx->L) - 2 * n + 1;
    for( i = 0; i < n; i++ )
    {
        const int value = first+2*i;
        lua_getfield(ctx->L,value+1,"#kind");
        lua_getfield(ctx->L,value+1,"#type");
        if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_string )
            error(ctx, row, col,"expecting one or more arguments of type string" );
        lua_pop(ctx->L,2);
        lua_pushvalue(ctx->L,value);
    }
    lua_concat(ctx->L,n);
    if( !ctx->skipMode )
    {
        BSRowCol loc;
        loc.row = row;
        loc.col = col;
        va_list args;
        va_end(args);
        switch( kind )
        {
        case 2:
            ctx->logger(BS_Error,ctx->loggerData,labelOrFilepath(ctx), loc, lua_tostring(ctx->L,-1), args);
            lua_pushnil(ctx->L);
            lua_error(ctx->L);
            break;
        case 1:
            ctx->logger(BS_Warning,ctx->loggerData,labelOrFilepath(ctx), loc, lua_tostring(ctx->L,-1), args);
            break;
        default:
            ctx->logger(BS_Info,ctx->loggerData,ctx->label, loc, lua_tostring(ctx->L,-1), args);
            break;
        }
    }

    lua_pop(ctx->L,1);

    lua_pushnil(ctx->L); // no return value and type
    lua_pushnil(ctx->L);
    BS_END_LUA_FUNC(ctx);
}

static void set_defaults(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 2 )
        error(ctx, row, col,"expecting two arguments" );

    const int arg1 = lua_gettop(ctx->L) - 4 + 1;
    const int arg2 = arg1 + 2;

    lua_getfield(ctx->L,ctx->builtins,"CompilerType");
    lua_getfield(ctx->L,arg1+1,"#kind");
    lua_getfield(ctx->L,arg1+1,"#type");
    if( !lua_equal(ctx->L,arg1+1,-3) &&
            ( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_symbol
                || !isInEnum(ctx, -3, arg1) ) )
        error(ctx, row, col,"first argument must be a CompilerType" );
    lua_pop(ctx->L,3);

    lua_getfield(ctx->L,ctx->builtins,"Config");
    const int cls = lua_gettop(ctx->L);
    lua_getfield(ctx->L,arg2+1,"#kind");
    if( lua_tointeger(ctx->L,-1) != BS_ClassDecl || !isSameOrSubclass(ctx,cls, arg2+1) )
        error(ctx, row, col,"second argument must be a Config instance");
    lua_pop(ctx->L,2);

    lua_getfield(ctx->L,ctx->builtins,"#inst");
    const int binst = lua_gettop(ctx->L);

    lua_getfield(ctx->L,binst,"#ctdefaults");
    assert( lua_istable(ctx->L,-1) );
    const int ctdefs = lua_gettop(ctx->L);

    if( !ctx->skipMode )
    {
        lua_pushvalue(ctx->L, arg1 );
        lua_pushvalue(ctx->L, arg2 );
        lua_rawset(ctx->L,ctdefs);
    }

    lua_pop(ctx->L,2);

    lua_pushnil(ctx->L); // no return value and type
    lua_pushnil(ctx->L);
    BS_END_LUA_FUNC(ctx);
}

static int checkListType(BSParserContext* ctx, int n, int t)
{
    lua_getfield(ctx->L,-1,"#kind");
    const int k = lua_tointeger(ctx->L,n);
    lua_pop(ctx->L,1);
    if( k != BS_ListType )
        return 0;
    lua_getfield(ctx->L,n,"#type");
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    const int err = lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != t;
    lua_pop(ctx->L,3);
    return !err;
}

static void trycompile(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n < 1 )
        error(ctx, row, col,"expecting at least one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_string )
        error(ctx, row, col,"expecting at least one argument of string type" );
    lua_pop(ctx->L,2);
    const int first = lua_gettop(ctx->L) - 2 * n + 1;

    lua_getfield(ctx->L,ctx->builtins,"#inst");
    const int binst = lua_gettop(ctx->L);

    lua_getfield(ctx->L,binst,"root_build_dir");
    const int rootOutDir = lua_gettop(ctx->L);

    lua_pushstring(ctx->L,"./_trycompile_.c");
    const int tmppath = lua_gettop(ctx->L);
    const int res = bs_add_path(ctx->L,rootOutDir,tmppath);
    assert(res==0);
    lua_replace(ctx->L,tmppath);

    if( !ctx->skipMode )
    {
        if( !bs_exists(lua_tostring(ctx->L,rootOutDir)) )
        {
            if( bs_mkdir(lua_tostring(ctx->L,rootOutDir)) != 0 )
                error(ctx, row, col,"error creating directory %s", lua_tostring(ctx->L,rootOutDir));
        }

        FILE* tmp = bs_fopen(bs_denormalize_path(lua_tostring(ctx->L,tmppath)),"w");
        if( tmp == NULL )
            error(ctx, row, col,"cannot create temporary file %s", lua_tostring(ctx->L,tmppath) );
        const int codelen = lua_objlen(ctx->L,first);
        fwrite(lua_tostring(ctx->L,first),1,codelen,tmp);
        fclose(tmp);
    }

    lua_getfield(ctx->L,binst,"target_toolchain");
    const int ts = lua_gettop(ctx->L);

    lua_getfield(ctx->L,binst,"host_os");
    const int os = lua_gettop(ctx->L);

    lua_pushstring(ctx->L,"");
    const int cflags = lua_gettop(ctx->L);
    lua_pushstring(ctx->L,"");
    const int defines = lua_gettop(ctx->L);
    lua_pushstring(ctx->L,"");
    const int includes = lua_gettop(ctx->L);

    lua_getfield(ctx->L,ctx->module.table, "#dir");
    const int dir = lua_gettop(ctx->L);

    if( n >= 2 )
    {
        // TODO: unify the following code with bsrunner addAll
        size_t i;
        if( !checkListType(ctx,first+1+1,BS_string) )
            error(ctx, row, col,"expecting argument 2 of string list type" );
        for( i = 1; i <= lua_objlen(ctx->L,first+1); i++ )
        {
            lua_rawgeti(ctx->L,first+1,i);
            lua_pushvalue(ctx->L,defines);
            if( strstr(lua_tostring(ctx->L,-2),"\\\"") != NULL )
                lua_pushfstring(ctx->L," \"-D%s\" ", lua_tostring(ctx->L,-2)); // strings can potentially include whitespace, thus quotes
            else
                lua_pushfstring(ctx->L," -D%s ", lua_tostring(ctx->L,-2));
            lua_concat(ctx->L,2);
            lua_replace(ctx->L,defines);
            lua_pop(ctx->L,1); // def
        }
        if( n >= 3 )
        {
            if( !checkListType(ctx,first+2+1,BS_path) )
                error(ctx, row, col,"expecting argument 3 of path list type" );
            for( i = 1; i <= lua_objlen(ctx->L,first+2); i++ )
            {
                lua_rawgeti(ctx->L,first+2,i);
                const int path = lua_gettop(ctx->L);
                if( *lua_tostring(ctx->L,-1) != '/' )
                {
                    // relative path
                    if( bs_add_path(ctx->L,dir,path) != 0 )
                        error(ctx, row, col,"error converting to absolute path" );
                    lua_replace(ctx->L,path);
                }
                lua_pushvalue(ctx->L,includes);
                lua_pushfstring(ctx->L," -I\"%s\" ", bs_denormalize_path(lua_tostring(ctx->L,path)) );
                lua_concat(ctx->L,2);
                lua_replace(ctx->L,includes);
                lua_pop(ctx->L,1); // path
            }
            if( n == 4 )
            {
                if( !checkListType(ctx,first+3+1,BS_string) )
                    error(ctx, row, col,"expecting argument 3 of string list type" );
                for( i = 1; i <= lua_objlen(ctx->L,first+3); i++ )
                {
                    lua_pushvalue(ctx->L,cflags);
                    lua_pushstring(ctx->L," ");
                    lua_rawgeti(ctx->L,first+3,i);
                    lua_concat(ctx->L,3);
                    lua_replace(ctx->L,cflags);
                }
            }else if( n > 4)
                error(ctx, row, col,"expecting one to four arguments" );
        }
    }

    if( strcmp( lua_tostring(ctx->L,ts), "msvc") == 0 )
        lua_pushstring(ctx->L,"cl /nologo /c ");
    else
    {
        lua_pushvalue(ctx->L,ts); // RISK
        lua_pushstring(ctx->L," -c ");
        lua_concat(ctx->L,2);
    }
    const int cmd = lua_gettop(ctx->L);

    lua_pushvalue(ctx->L,cmd);
    lua_pushvalue(ctx->L,cflags);
    lua_pushvalue(ctx->L,includes);
    lua_pushvalue(ctx->L,defines);
    lua_pushfstring(ctx->L," %s",bs_denormalize_path(lua_tostring(ctx->L,tmppath)));
    if( strcmp(lua_tostring(ctx->L,os),"win32")==0 ||
            strcmp(lua_tostring(ctx->L,os),"msdos")==0 ||
            strcmp(lua_tostring(ctx->L,os),"winrt")==0 )
        lua_pushstring(ctx->L," 2> nul");
    else
        lua_pushstring(ctx->L," 2>/dev/null");
    lua_concat(ctx->L,6);
    lua_replace(ctx->L,cmd);

    int res2 = 0;

    if( !ctx->skipMode )
        res2 = !bs_exec(lua_tostring(ctx->L,cmd)); // works for all gcc, clang and cl

    lua_pop(ctx->L,10); // binst, rootOutDir, tmppath, ts, os, cflags, defines, includes, dir, cmd

    lua_pushboolean(ctx->L,res2);
    lua_getfield(ctx->L,ctx->builtins,"bool");

    BS_END_LUA_FUNC(ctx);
}

static void vardecl(BSParserContext* ctx, BSScope* scope);
static void condition(BSParserContext* ctx, BSScope* scope);
static void assigOrCall(BSParserContext* ctx, BSScope* scope);

static void evalInst(BSParserContext* ctx, BSScope* scope)
{
    // in: declaration
    BS_BEGIN_LUA_FUNC(ctx,0);
    const int templ = lua_gettop(ctx->L);

    BSToken t = nextToken(ctx);
    if( t.tok != Tok_Lpar)
        error(ctx, t.loc.row, t.loc.col,"expecting '('" );
    BSToken lpar = t;
    bslex_cursetref(ctx->lex); // lpar will be the pos shown in lexer stack

    size_t n = 0;
    BSTokChain* start = 0;
    BSTokChain* last = 0;
    int parLevel = 1;
    BSToken loc = lpar;
    while( 1 )
    {
        t = bslex_hnext(ctx->lex); // NOTE that these tokens come from different strings;
                                   // we cannot simply take the string between the first and last token
        if( t.tok == Tok_Lpar )
            parLevel++;
        else if( t.tok == Tok_Rpar )
            parLevel--;
        // NOTE parser will report error in case of unbalanced ')', so parLevel is never < 0
        if( !(parLevel == 0 && t.tok == Tok_Rpar) && t.tok != Tok_Comma )
        {
            BSTokChain* ts = (BSTokChain*)malloc(sizeof(BSTokChain));
            ts->tok = t;
            ts->next = 0;
            if( start == 0 )
                start = last = ts;
            else
            {
                last->next = ts;
                last = ts;
            }
        }
        if( ( parLevel == 0 && t.tok == Tok_Rpar ) || t.tok == Tok_Comma )
        {
            if( start )
            {
                n++;
                BSTokChain* ts = (BSTokChain*)malloc(sizeof(BSTokChain));
                ts->tok = loc;
                ts->tok.tok = 0;
                ts->next = start;
                lua_pushlightuserdata(ctx->L,ts);
                start = last = 0;
            }
            if( t.tok == Tok_Rpar )
                break;
            loc = t;
        }else if( t.tok == Tok_Eof )
            break;
    }
    if( t.tok != Tok_Rpar )
        error(ctx, lpar.loc.row, lpar.loc.col,"argument list not terminated" );
    if( n != lua_objlen(ctx->L,templ) )
        error(ctx, t.loc.row, t.loc.col,"number of actual doesn't fit number of formal arguments" );

    lua_getfield(ctx->L,templ,"#code");
    const int code = lua_gettop(ctx->L);

    lua_getfield(ctx->L,templ,"#brow");
    lua_getfield(ctx->L,templ,"#bcol");
    const BSRowCol orig = { lua_tointeger(ctx->L,-2), lua_tointeger(ctx->L,-1) };
    lua_pop(ctx->L,2);
    lua_getfield(ctx->L,templ,"#source");
    const int source = lua_gettop(ctx->L);

    if( !ctx->skipMode )
    {
        if( bslex_hopen(ctx->lex,lua_tostring(ctx->L,code), lua_objlen(ctx->L,code),
                        lua_tostring(ctx->L,source), orig) != 0 )
            return;
        t = nextToken(ctx);
        if( t.tok != Tok_Lbrace )
            error(ctx, t.loc.row, t.loc.col,"internal error" );

        size_t i;
        for( i = 1; i <= n; i++ )
        {
            lua_rawgeti(ctx->L,templ,i); // name
            BSTokChain* arg = (BSTokChain*)lua_topointer(ctx->L,templ+i);
            bslex_addarg(ctx->lex,lua_tostring(ctx->L,-1), arg);
            lua_pop(ctx->L,1); // name
        }
        if( n )
            lua_pop(ctx->L, n);

        t = peekToken(ctx,1);
        while( !endOfBlock(&t,0) && t.tok != Tok_Eof )
        {
            if( ( t.tok == Tok_subdir || t.tok == Tok_submod || t.tok == Tok_submodule ) && &ctx->module == scope )
                // Tok_subdir is deprecated
                submodule(ctx,t.tok == Tok_subdir);
            else
                switch( t.tok )
                {
                case Tok_var:
                case Tok_let:
                case Tok_param:
                    vardecl(ctx, scope);
                    break;
                case Tok_type:
                    typedecl(ctx, scope);
                    break;
                case Tok_if:
                    condition(ctx, scope);
                    if( ctx->fullAst )
                        lua_rawseti(ctx->L,scope->table,++scope->n);
                    else
                        lua_pop(ctx->L,1);
                    break;
                case Tok_Hat:
                case Tok_Dot:
                case Tok_ident:
                    assigOrCall(ctx,scope);
                    break;
                default:
                    unexpectedToken(ctx, &t,"in macro body");
                    break;
                }

            t = peekToken(ctx,1);
            if( t.tok == Tok_Semi )
            {
                nextToken(ctx); // eat it
                t = peekToken(ctx,1);
            }
        }
        t = nextToken(ctx);
        if( t.tok != Tok_Rbrace )
            error(ctx, t.loc.row, t.loc.col,"internal error" );
    }else if( n )
        lua_pop(ctx->L, n);

    lua_pop(ctx->L,2); // code, source
    BS_END_LUA_FUNC(ctx);
}

static void evalCall(BSParserContext* ctx, BSScope* scope)
{
    // in: proc declaration
    BS_BEGIN_LUA_FUNC(ctx,1); // out: value, type
    const int proc = lua_gettop(ctx->L); // becomes value
    lua_pushnil(ctx->L); // becomes type

    BSToken t = nextToken(ctx);
    if( t.tok != Tok_Lpar)
        error(ctx, t.loc.row, t.loc.col,"expecting '('" );
    BSToken lpar = t;

    lua_getfield(ctx->L,proc,"#kind");
    const int kind = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    if( kind != BS_ProcDef )
        error(ctx, lpar.loc.row, lpar.loc.col,"the designated object is not callable" );

    t = peekToken(ctx,1);
    int n = 0;
    while( t.tok != Tok_Rpar && t.tok != Tok_Eof )
    {
        expression(ctx,scope,0);
        n++;
        t = peekToken(ctx,1);
        if( t.tok == Tok_Comma )
        {
            nextToken(ctx);
            t = peekToken(ctx,1);
        }
    }
    if( t.tok == Tok_Rpar )
        nextToken(ctx);
    else
        error(ctx, lpar.loc.row, lpar.loc.col,"argument list not terminated" );

    lua_getfield(ctx->L,proc,"#id");
    const int id = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);

    switch( id )
    {
    case 1:
        samelist(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 2:
        sameset(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 3:
        toint(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 4:
        toreal(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 5:
        tostring(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 10:
        print(ctx,n,lpar.loc.row, lpar.loc.col,0);
        break;
    case 9:
        print(ctx,n,lpar.loc.row, lpar.loc.col,1);
        break;
    case 8:
        print(ctx,n,lpar.loc.row, lpar.loc.col,2);
        break;
    case 11: // dump
        {
            if( n == 0 || n > 2 )
                error(ctx, lpar.loc.row, lpar.loc.col,"expecting one or two arguments" );
            if( !ctx->skipMode )
            {
                if( n == 2 )
                {
                    dump2(ctx,lua_tostring(ctx->L,lua_gettop(ctx->L)-2+1),lua_gettop(ctx->L)-4+1);
                }else
                    dump(ctx,lua_gettop(ctx->L)-2+1);
            }
            lua_pushnil(ctx->L);
            lua_pushnil(ctx->L);
        }
        break;
    case 6:
        topath(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 12:
        abspath(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 13:
        relpath(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 14:
        readstring(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 15:
        trycompile(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 16:
        build_dir(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 17:
        modname(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    case 18:
        set_defaults(ctx,n,lpar.loc.row, lpar.loc.col);
        break;
    default:
        error(ctx, lpar.loc.row, lpar.loc.col,"procedure not yet implemented" );
    }

    lua_replace(ctx->L,proc+1);
    lua_replace(ctx->L,proc);
    lua_pop(ctx->L,n*2);

    BS_END_LUA_FUNC(ctx);
}


// returns 0: no list or incompatible; 1: both list; 2: left list; 3: right list
static int isListAndElemType( BSParserContext* ctx, int lhst, int rhst, int lhsv, int rhsv )
{
    // lhs is list and rhs is list or element or vice versa; lhs and rhs point to types
    const int top = lua_gettop(ctx->L);
    if( lhst <= 0 )
        lhst += top + 1;
    if( rhst <= 0 )
        rhst += top + 1;
    if( !lua_istable(ctx->L,lhst) || !lua_istable(ctx->L,rhst) )
        return 0;
    lua_getfield(ctx->L,lhst,"#kind");
    const int klhs = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    lua_getfield(ctx->L,rhst,"#kind");
    const int krhs = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    if( klhs != BS_ListType && krhs != BS_ListType )
        return 0;
    if( klhs == BS_ListType && krhs == BS_ListType && sameType(ctx,lhst,rhst) )
        return 1; // both lists
    if( klhs == BS_ListType )
    {
        lua_getfield(ctx->L,lhst,"#type");
        const int res = sameType(ctx,-1,rhst) || isSameOrSubclass(ctx, -1, rhst) || isInEnum(ctx,-1,rhsv);
        lua_pop(ctx->L,1);
        return res ? 2 : 0; // lhs is list, rhs is element
    }
    if( krhs == BS_ListType )
    {
        lua_getfield(ctx->L,rhst,"#type");
        const int res = sameType(ctx,lhst,-1) || isSameOrSubclass(ctx, lhst, -1) || isInEnum(ctx,-1, lhsv);
        lua_pop(ctx->L,1);
        return res ? 3 : 0; // rhs is list, lhs is element
    }
    return 0;
}

static void evalIfExpr(BSParserContext* ctx, BSScope* scope, BSToken* qmark, int lhsType)
{
    // in: value, type of if expression
    BS_BEGIN_LUA_FUNC(ctx,0); // out: value, type
    lua_getfield(ctx->L,-1,"#type");
    if( lua_tointeger(ctx->L,-1) != BS_boolean )
        error(ctx, qmark->loc.row, qmark->loc.col,"expecting a boolean expression left of '?'" );
    lua_pop(ctx->L,1);

    if( ctx->skipMode )
    {
        lua_pop(ctx->L,2); // value, type
        expression(ctx,scope,lhsType);
        BSToken t = nextToken(ctx);
        if( t.tok != Tok_Colon )
            error(ctx, t.loc.row, t.loc.col,"expecting ':'" );
        expression(ctx,scope,lhsType);
        // stack: value, type, value, type
        if( !sameType(ctx, -1,-3) )
            error(ctx, t.loc.row, t.loc.col,"expression left and right of ':' must be of same type" );
        lua_pop(ctx->L,2); // value, type
        // here the value, type of the first expression survives
    }else
    {
        const int cond = lua_toboolean(ctx->L,-2);
        lua_pop(ctx->L,2); // value, type
        if( cond )
        {
            expression(ctx,scope,lhsType);
            BSToken t = nextToken(ctx);
            if( t.tok != Tok_Colon )
                error(ctx, t.loc.row, t.loc.col,"expecting ':'" );
            ctx->skipMode = 1;
            expression(ctx,scope,lhsType);
            ctx->skipMode = 0;
            if( !sameType(ctx, -1,-3) )
                error(ctx, t.loc.row, t.loc.col,"expression left and right of ':' must be of same type" );
            lua_pop(ctx->L,2); // value, type
        }else
        {
            ctx->skipMode = 1;
            expression(ctx,scope,lhsType);
            ctx->skipMode = 0;
            BSToken t = nextToken(ctx);
            if( t.tok != Tok_Colon )
                error(ctx, t.loc.row, t.loc.col,"expecting ':'" );
            expression(ctx,scope,lhsType);
            if( !sameType(ctx, -1,-3) )
                error(ctx, t.loc.row, t.loc.col,"expression left and right of ':' must be of same type" );
            lua_remove(ctx->L,-3);
            lua_remove(ctx->L,-3);
        }
    }
    BS_END_LUA_FUNC(ctx);
}

static void evalListLiteral(BSParserContext* ctx, BSScope* scope, BSToken* lbrack, int lhsType)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    BSToken t = peekToken(ctx,1);
    assert(lbrack);
    if( lhsType )
        assert( lua_istable(ctx->L,lhsType) );
    if( t.tok == Tok_Rbrack || lbrack->tok == Tok_LbrackRbrack )
    {
        // empty list
        nextToken(ctx); // eat it
        lua_createtable(ctx->L,0,0);
        if( lhsType )
        {
            lua_getfield(ctx->L,lhsType,"#kind");
            const int k = lua_tointeger(ctx->L,-1);
            if( k != BS_ListType )
                error(ctx, lbrack->loc.row, lbrack->loc.col,"incompatible type" );
            lua_pop(ctx->L,1);
            lua_pushvalue(ctx->L,lhsType);
        }else
            error(ctx, lbrack->loc.row, lbrack->loc.col,"cannot determine list type" );
    }else
    {
        int n = 0;
        lua_createtable(ctx->L,0,0);
        const int list = lua_gettop(ctx->L);
        expression(ctx,scope,0);
        // value, type
        const int expType = lua_gettop(ctx->L);
        if( lua_isnil(ctx->L,expType) )
            error(ctx, lbrack->loc.row, lbrack->loc.col,"cannot determine list type" );
        const int expVal = expType - 1;
        lua_getfield(ctx->L,expType,"#kind");
        const int kr = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);

        if( lhsType )
        {
            lua_getfield(ctx->L,lhsType,"#kind");
            int kl = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            if( kl == BS_ListType )
            {
                lua_getfield(ctx->L,lhsType,"#type");
                if( !isSameOrSubclass(ctx,-1,expType) && !isInEnum(ctx,-1,expVal) && !sameType(ctx,-1,expType) )
                    error(ctx, t.loc.row, t.loc.col,"the element is not compatible with the list type" );
                // replace the type of the first expression by lhsType
                lua_replace(ctx->L,expType);
            }// else: the assignment type check produces the error
        }

        // store the first element in the list
        lua_pushvalue(ctx->L,expVal);
        lua_rawseti(ctx->L,list,++n);
        lua_remove(ctx->L,expVal);
        // take the type of the first element as reference
        const int refType = lua_gettop(ctx->L);

        t = peekToken(ctx,1);
        if( t.tok == Tok_Comma )
        {
            nextToken(ctx); // ignore comma
            t = peekToken(ctx,1);
        }
        while( t.tok != Tok_Rbrack && t.tok != Tok_Eof )
        {
            expression(ctx,scope,0);
            if( !sameType(ctx,refType,-1) && !isSameOrSubclass(ctx,refType,-1) && !isInEnum(ctx,refType,-2) )
                error(ctx, t.loc.row, t.loc.col,"all elements of the list literal must have compatible types" );
            lua_pushvalue(ctx->L,-2);
            lua_rawseti(ctx->L,list,++n);
            lua_pop(ctx->L,2);

            t = peekToken(ctx,1);
            if( t.tok == Tok_Comma )
            {
                nextToken(ctx); // ignore comma
                t = peekToken(ctx,1);
            }
        }
        if( t.tok == Tok_Eof )
            error(ctx, lbrack->loc.row, lbrack->loc.col,"non terminated array literal" );
        else
            nextToken(ctx); // eat rbrack
        // stack: list, element type
        lua_createtable(ctx->L,0,0);
        lua_pushinteger(ctx->L,BS_ListType);
        lua_setfield(ctx->L,-2, "#kind" );
        lua_pushvalue(ctx->L,refType);
        lua_setfield(ctx->L,-2, "#type" );
        lua_remove(ctx->L,-2);
        // stack: list, list type
    }
    BS_END_LUA_FUNC(ctx);
}

static void push_unescaped(lua_State* L, const char* utf8, int len )
{
#if 0
    // NOTE: this code works, but we need to send escaped strings to the build tools, so just keep the escapes for now
    char* tmp = (char*)malloc(len);
    char* p = tmp;
    uchar n;
    int escape = 0;
    while( len )
    {
        const uint ch = unicode_decode_utf8((const uchar*)utf8,&n);
        if( ch == '\\' )
            escape = 1;
        else
        {
            if( escape )
            {
                if( ch == '"' || ch == '\\' )
                    // true escape
                    *p++ = ch;
                else
                {
                    // not an escape
                    *p++ = '\\';
                    *p++ = ch;
                }
                escape = 0;
            }else
            {
                memcpy(p,utf8,n);
                p += n;
            }
        }
        len -= n;
        utf8 += n;
    }
    lua_pushlstring(L,tmp, p-tmp);

    free(tmp);
#else
    lua_pushlstring(L,utf8,len);
#endif
}

static int factor(BSParserContext* ctx, BSScope* scope, int lhsType)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // value, type
    BSToken t = peekToken(ctx,1);
    int ret = -1;
    switch( t.tok )
    {
    case Tok_integer:
        nextToken(ctx);
        lua_pushlstring(ctx->L, t.val, t.len);
        lua_pushinteger(ctx->L,lua_tointeger(ctx->L, -1 ));
        lua_replace(ctx->L,-2);
        lua_getfield(ctx->L,ctx->builtins,"int");
        break;
    case Tok_real:
        nextToken(ctx);
        lua_pushlstring(ctx->L, t.val, t.len);
        lua_pushnumber(ctx->L, lua_tonumber(ctx->L, -1 ));
        lua_replace(ctx->L,-2);
        lua_getfield(ctx->L,ctx->builtins,"real");
        break;
    case Tok_true:
    case Tok_false:
        nextToken(ctx);
        lua_pushboolean(ctx->L,t.tok == Tok_true);
        lua_getfield(ctx->L,ctx->builtins,"bool");
        break;
    case Tok_string:
        nextToken(ctx);
        push_unescaped(ctx->L, t.val+1, t.len-2); // remove ""
        lua_getfield(ctx->L,ctx->builtins,"string");
        break;
    case Tok_symbol:
        nextToken(ctx);
        lua_pushlstring(ctx->L, t.val+1, t.len-1); // remove leading `
        lua_getfield(ctx->L,ctx->builtins,"symbol");
        break;
    case Tok_path:
        {
            nextToken(ctx);
            const char* path = t.val;
            int len = t.len;
            if( *path == '\'' )
            {
                path++;
                len -= 2;
            }
            assert( len > 0 );
            int prefix = 0;
            if( *path != '/' && *path != '.' )
            {
                // a relative path directly starting with fname
                // we normalize by prefixing './'
                prefix = 1;
            }
            if( prefix )
                lua_pushstring(ctx->L,"./");
            lua_pushlstring(ctx->L, path, len);
            if( prefix )
                lua_concat(ctx->L,2);
            lua_getfield(ctx->L,ctx->builtins,"path");
            // result is an unquoted '.' | '..' | ('../' | './' | '//') fsname { '/' fsname }
        }
        break;
    case Tok_Hat:
    case Tok_Dot:
    case Tok_ident:
        {
            int bound = 0;
            ret = resolveInstance(ctx,scope);
            if( bound )
                lua_remove(ctx->L,bound);

            t = peekToken(ctx,1);
            if( t.tok == Tok_Lpar )
            {
                lua_remove(ctx->L,-2);
                // stack: derefed declaration
                evalCall(ctx,scope);
                if( lua_isnil(ctx->L,-1) )
                    error(ctx, t.loc.row, t.loc.col,"cannot call this procedure like a function" );
            }else
            {
                // resolved container instance + derefed declaration
                const int value = lua_gettop(ctx->L) - 2 + 1;
                lua_getfield(ctx->L,value+1,"#name");
                lua_rawget(ctx->L,value);
                // resolved container instance + derefed declaration + value
                lua_getfield(ctx->L,value+1,"#type");
                // resolved container instance + derefed declaration + value + type
                lua_remove(ctx->L,value);
                // resolved container instance + value + type
                lua_remove(ctx->L,value);
                // value, type
           }
        }
        break;
    case Tok_Lpar:
        nextToken(ctx);
        expression(ctx,scope,lhsType);
        t = peekToken(ctx,1);
        if( t.tok == Tok_Qmark )
        {
            // peek '?' and eval condition expression
            nextToken(ctx);
            evalIfExpr(ctx,scope,&t, lhsType);
        }
        t = nextToken(ctx);
        if( t.tok != Tok_Rpar )
            error(ctx, t.loc.row, t.loc.col,"expecting ')' here" );
        break;
    case Tok_Plus:
    case Tok_Minus:
    case Tok_Bang:
        nextToken(ctx);
        factor(ctx,scope,lhsType);
        if( t.tok == Tok_Plus || t.tok == Tok_Minus )
        {
            lua_getfield(ctx->L,-1,"#kind");
            const int k = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            lua_getfield(ctx->L,-1,"#type");
            const int b = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            if( k != BS_BaseType || ( b != BS_integer && b != BS_real ) )
                error(ctx, t.loc.row, t.loc.col,"unary operator only applicable to integer or real types" );
            if( t.tok == Tok_Minus )
            {
                lua_pushnumber(ctx->L, -lua_tonumber(ctx->L, -2));
                lua_replace(ctx->L, -3);
            }
        }else
        {
            lua_getfield(ctx->L,-1,"#kind");
            const int k = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            lua_getfield(ctx->L,-1,"#type");
            const int b = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            if( k != BS_BaseType || b != BS_boolean )
                error(ctx, t.loc.row, t.loc.col,"unary operator only applicable to boolean types" );
            lua_pushboolean(ctx->L, !lua_toboolean(ctx->L, -2));
            lua_replace(ctx->L, -3);
        }
        break;
    case Tok_Lbrack:
    case Tok_LbrackRbrack:
        nextToken(ctx);
        evalListLiteral(ctx,scope,&t,lhsType);
        break;
    default:
        unexpectedToken(ctx,&t,"in factor");
        break;
    }

    BS_END_LUA_FUNC(ctx);
    return ret;
}

static void evalMulOp(BSParserContext* ctx, BSToken* tok)
{
    // in: // value, type, value, type
    BS_BEGIN_LUA_FUNC(ctx,-2); // value, type
    const int lhs = lua_gettop(ctx->L) - 4 + 1;
    const int rhs = lua_gettop(ctx->L) - 2 + 1;
    const int l = isListAndElemType(ctx,-3,-1,0,0);
    if( l )
    {
        const int nl = l == 1 || l == 2 ? lua_objlen(ctx->L,lhs) : 0;
        const int nr = l == 1 || l == 3 ? lua_objlen(ctx->L,rhs) : 0;
        if( tok->tok == Tok_Star )
        {
            if( l != 1 && l != 2 )
                error(ctx, tok->loc.row, tok->loc.col,"only list * list or list * element supported" );
            lua_createtable(ctx->L,nl,0);
            const int res = lua_gettop(ctx->L);
            int i = 0, n = 0;
            if( l == 2 )
            {
                // add element, only if not yet included
                int found = 0;
                for( i = 1; i <= nl; i++ )
                {
                    // copy all elements from lhs to res and see whether rhs is included
                    lua_rawgeti(ctx->L,lhs,i);
                    found = found || lua_equal(ctx->L,-1,rhs);
                    lua_rawseti(ctx->L,res, ++n);
                }
                if( !found )
                {
                    lua_pushvalue(ctx->L,rhs);
                    lua_rawseti(ctx->L,res, ++n);
                }
            }else
            {
                // intersection of two lists
                lua_createtable(ctx->L,nr + 1,0); // CHECK: this is barely efficient
                const int tmp = lua_gettop(ctx->L);
                for( i = 1; i <= nr; i++ )
                {
                    lua_rawgeti(ctx->L,rhs,i);
                    lua_pushvalue(ctx->L,-1);
                    lua_rawset(ctx->L,tmp);
                }
                for( i = 1; i <= nl; i++ )
                {
                    lua_rawgeti(ctx->L,lhs,i);
                    lua_rawget(ctx->L,tmp);
                    const int found = !lua_isnil(ctx->L,-1);
                    lua_pop(ctx->L,1);
                    if( found )
                    {
                        lua_rawgeti(ctx->L,lhs,i);
                        lua_rawseti(ctx->L,res,++n);
                    }
                }
                lua_pop(ctx->L,1); // tmp
            }
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,2);
        }else
            error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to list operand type" );
    }else if( !sameType(ctx,lhs+1, rhs+1) )
        error(ctx, tok->loc.row, tok->loc.col,"operator requires the same type on both sides" );
    else
    {
        lua_getfield(ctx->L,-1,"#kind");
        const int k = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        if( k != BS_BaseType )
            error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
        lua_getfield(ctx->L,-1,"#type");
        const int t = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        switch( t )
        {
        case BS_boolean:
            if( tok->tok == Tok_2Amp )
                lua_pushboolean(ctx->L, lua_toboolean(ctx->L,lhs) && lua_toboolean(ctx->L,rhs) );
            else
                error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to boolean operands" );
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,2);
            break;
        case BS_integer:
        case BS_real:
            switch(tok->tok)
            {
            case Tok_Star:
                lua_pushnumber(ctx->L, lua_tonumber(ctx->L,lhs) * lua_tonumber(ctx->L,rhs));
                break;
            case Tok_Slash:
                lua_pushnumber(ctx->L, lua_tonumber(ctx->L,lhs) / lua_tonumber(ctx->L,rhs));
                break;
            case Tok_Percent:
                lua_pushnumber(ctx->L, lua_tointeger(ctx->L,lhs) % lua_tointeger(ctx->L,rhs));
                break;
            default:
                error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to number operands" );
                break;
            }
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,2);
            break;
        default:
            error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
            break;
        }
    }
    BS_END_LUA_FUNC(ctx);
}

static int term(BSParserContext* ctx, BSScope* scope, int lhsType)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // value, type
    int ret = factor(ctx,scope, lhsType);
    BSToken t = peekToken(ctx,1);
    while( t.tok == Tok_Star || t.tok == Tok_Slash || t.tok == Tok_2Amp || t.tok == Tok_Percent )
    {
        nextToken(ctx);
        factor(ctx,scope,lhsType);
        evalMulOp(ctx,&t);
        t = peekToken(ctx,1);
        ret = -1;
    }
    BS_END_LUA_FUNC(ctx);
    return ret;
}

static void addPath(BSParserContext* ctx, BSToken* tok, int lhs, int rhs)
{
    BS_BEGIN_LUA_FUNC(ctx,1); // new path
    const int res = bs_add_path(ctx->L,lhs,rhs);
    switch( res )
    {
    case 1:
        error(ctx, tok->loc.row, tok->loc.col,"right side cannot be an absolute path");
        break;
    case 2:
        error(ctx, tok->loc.row, tok->loc.col,"right side cannot be appended to given left side" );
        break;
    }
    BS_END_LUA_FUNC(ctx);
}

int bs_add_path(lua_State* L, int lhs, int rhs)
{
    // doesn't touch lhs nor rhs, and pushes a value on the stack, unless non-zero returned
    const char* lstr = lua_tostring(L,lhs);
    const char* rstr = lua_tostring(L,rhs);
    if( strncmp(rstr,"//", 2) == 0 )
        return 1; // right side cannot be an absolute path
    int llen = strlen(lstr);

    if( strcmp(lstr,".") == 0 )
    {
        // if lhs="." then result is rhs
        lua_pushvalue(L,rhs);
        return 0;
    }

    if( strncmp(rstr,"..", 2 ) == 0 )
    {
        // TODO: this treats root windows paths like unix paths, i.e. //c: + ../x becomes //x
        rstr += 2;
        assert(lstr[llen-1] != '/');
        while( llen >= 0 )
        {
            if( lstr[--llen] == '/' )
                break;
        }
        if( llen < 1 ) // < 1 to avoid using // as a segment
            return 2;
        while( strncmp(rstr,"/..", 3 ) == 0 )
        {
            rstr += 3;
            while( llen >= 0 )
            {
                if( lstr[--llen] == '/' )
                    break;
            }
            if( llen < 1 )
            {
                // rhs exhausts lhs
                return 2; // right side cannot be appended to given left side
            }
        }
        lua_pushlstring(L,lstr,llen);
    }else if( strncmp(rstr,".",1) == 0 )
    {
        lua_pushstring(L,lstr);
        rstr += 1;
    }else
        assert(0); // not allowed since factor produces unified path syntax

    lua_pushstring(L,rstr);
    lua_concat(L,2);
    return 0;
}

static void evalAddOp(BSParserContext* ctx, BSToken* tok)
{
    // in: // value, type, value, type
    BS_BEGIN_LUA_FUNC(ctx,-2); // value, type
    const int lhs = lua_gettop(ctx->L) - 4 + 1;
    const int rhs = lua_gettop(ctx->L) - 2 + 1;
    const int l = isListAndElemType(ctx,-3,-1,0,0);
    if( l )
    {
        const int nl = l == 1 || l == 2 ? lua_objlen(ctx->L,lhs) : 0;
        const int nr = l == 1 || l == 3 ? lua_objlen(ctx->L,rhs) : 0;
        if( tok->tok == Tok_Plus )
        {
            lua_createtable(ctx->L,nl + nr + 1,0);
            const int res = lua_gettop(ctx->L);
            int i = 0, n = 0;
            // type compat was already checked by listAndElement
            for( i = 1; i <= nl; i++ )
            {
                lua_rawgeti(ctx->L,lhs,i);
                lua_rawseti(ctx->L,res,++n);
            }
            if( l == 3 )
            {
                lua_pushvalue(ctx->L,lhs);
                lua_rawseti(ctx->L,res,++n);
            }
            for( i = 1; i <= nr; i++ )
            {
                lua_rawgeti(ctx->L,rhs,i);
                lua_rawseti(ctx->L,res,++n);
            }
            if( l == 2 )
            {
                lua_pushvalue(ctx->L,rhs);
                lua_rawseti(ctx->L,res,++n);
            }
            if( l == 1 || l == 2 )
            {
                lua_replace(ctx->L,lhs);
                lua_pop(ctx->L,2);
            }else
            {
                lua_replace(ctx->L,rhs);
                lua_remove(ctx->L,-3);
                lua_remove(ctx->L,-3);
            }
        }else if( tok->tok == Tok_Minus )
        {
            if( l != 1 && l != 2 )
                error(ctx, tok->loc.row, tok->loc.col,"only list minus list or list minus element supported" );
            lua_createtable(ctx->L,nl,0);
            const int res = lua_gettop(ctx->L);
            int i = 0, n = 0;
            lua_createtable(ctx->L,nr + 1,0); // CHECK: this is barely efficient,
            const int tmp = lua_gettop(ctx->L);
            for( i = 1; i <= nr; i++ )
            {
                lua_rawgeti(ctx->L,rhs,i);
                lua_pushvalue(ctx->L,-1);
                lua_rawset(ctx->L,tmp);
            }
            if( l == 2 )
            {
                lua_pushvalue(ctx->L,rhs);
                lua_pushvalue(ctx->L,rhs);
                lua_rawset(ctx->L,tmp);
            }
            for( i = 1; i <= nl; i++ )
            {
                lua_rawgeti(ctx->L,lhs,i);
                lua_rawget(ctx->L,tmp);
                const int toRemove = !lua_isnil(ctx->L,-1);
                lua_pop(ctx->L,1);
                if( !toRemove )
                {
                    lua_rawgeti(ctx->L,lhs,i);
                    lua_rawseti(ctx->L,res,++n);
                }
            }
            lua_pop(ctx->L,1); // tmp
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,2);
        }else
            error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to list operand type" );
    }else if( !sameType(ctx,lhs+1, rhs+1) )
    {
        error(ctx, tok->loc.row, tok->loc.col,"operator requires the same type on both sides" );
    }else
    {
        lua_getfield(ctx->L,lhs+1,"#kind");
        const int k = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        if( k != BS_BaseType )
            error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
        lua_getfield(ctx->L,lhs+1,"#type");
        const int t = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        switch( t )
        {
        case BS_boolean:
            if( tok->tok == Tok_2Bar )
            {
                lua_pushboolean(ctx->L, lua_toboolean(ctx->L,lhs) || lua_toboolean(ctx->L,rhs) );
            }else
                error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to boolean operands" );
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,2);
            break;
        case BS_integer:
        case BS_real:
            switch(tok->tok)
            {
            case Tok_Plus:
                lua_pushnumber(ctx->L, lua_tonumber(ctx->L,lhs) + lua_tonumber(ctx->L,rhs));
                break;
            case Tok_Minus:
                lua_pushnumber(ctx->L, lua_tonumber(ctx->L,lhs) - lua_tonumber(ctx->L,rhs));
                break;
            default:
                error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to number operands" );
                break;
            }
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,2);
            break;
        case BS_string:
            if( tok->tok == Tok_Plus )
            {
                lua_pushvalue(ctx->L,lhs);
                lua_pushvalue(ctx->L,rhs);
                lua_concat(ctx->L,2);
                lua_replace(ctx->L,lhs);
                lua_pop(ctx->L,2);
            }else
                error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to string operands" );
            break;
        case BS_path:
            if( tok->tok == Tok_Plus )
            {
                addPath(ctx,tok,lhs,rhs);
                lua_replace(ctx->L,lhs);
                lua_pop(ctx->L,2);
            }else
                error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to path operands" );
            break;
        default:
            error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
            break;
        }
    }
    BS_END_LUA_FUNC(ctx);
}

static int SimpleExpression(BSParserContext* ctx, BSScope* scope, int lhsType)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // value, type
    int ret = term(ctx,scope,lhsType);
    BSToken t = peekToken(ctx,1);
    while( t.tok == Tok_Plus || t.tok == Tok_Minus || t.tok == Tok_2Bar )
    {
        nextToken(ctx);
        term(ctx,scope,lhsType);
        evalAddOp(ctx,&t);
        t = peekToken(ctx,1);
        ret = -1;
    }
    BS_END_LUA_FUNC(ctx);
    return ret;
}

static void checkAscii(BSParserContext* ctx, const char* str, BSToken* tok)
{
    while( *str != 0 )
    {
        if( *str & 0x80 )
            error(ctx, tok->loc.row, tok->loc.col,"comparison operator only applicable to ASCII strings");
        str++;
    }
}

static void evalRelation(BSParserContext* ctx, BSToken* tok)
{
    // in: // value, type, value, type
    BS_BEGIN_LUA_FUNC(ctx,-2); // value, type
    const int l = isListAndElemType(ctx,-3,-1,0,0);
    const int lhs = lua_gettop(ctx->L) - 4 + 1; // value
    const int rhs = lua_gettop(ctx->L) - 2 + 1; // value

    if( l )
    {
        // lists are compared by value, not by reference
        const int nl = l == 1 || l == 2 ? lua_objlen(ctx->L,lhs) : 0;
        const int nr = l == 1 || l == 3 ? lua_objlen(ctx->L,rhs) : 0;
        if( l == 3 && tok->tok == Tok_in )
        {
            // element in list
            int i;
            int eq = 0;
            for( i = 1; i <= nr; i++ )
            {
                lua_rawgeti(ctx->L,rhs,i);
                eq = lua_equal(ctx->L,-1,lhs);
                lua_pop(ctx->L,1);
                if( eq )
                    break;
            }
            lua_pushboolean(ctx->L, eq );
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,3);
            lua_getfield(ctx->L,ctx->builtins,"bool");
        }if( l == 1 && ( tok->tok == Tok_2Eq || tok->tok == Tok_BangEq ) )
        {
            // list a == list b ba reference
            int eq = lua_equal(ctx->L, lhs, rhs);
            lua_pushboolean(ctx->L, tok->tok == Tok_2Eq ? eq : !eq);
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,3);
            lua_getfield(ctx->L,ctx->builtins,"bool");
        }else
            error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to operand types" );
    }else if( !sameType(ctx,lhs+1, rhs+1) && !isInEnum(ctx,lhs+1,rhs) && !isInEnum(ctx,rhs+1,lhs) )
    {
        error(ctx, tok->loc.row, tok->loc.col,"operator requires the same base type on both sides" );
    }else
    {
        lua_getfield(ctx->L,-1,"#kind");
        const int k = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        if( ( k == BS_ModuleDef || k == BS_ClassDecl || k == BS_EnumDecl ) &&
                ( tok->tok == Tok_2Eq || tok->tok == Tok_BangEq ) )
        {
            const int eq = lua_equal(ctx->L,lhs,rhs);
            lua_pushboolean(ctx->L, tok->tok == Tok_2Eq ? eq : !eq );
        }else if( k != BS_BaseType )
            error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
        else
        {
            lua_getfield(ctx->L,-1,"#type");
            const int t = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            switch( t )
            {
            case BS_boolean:
            case BS_symbol:
                switch(tok->tok)
                {
                case Tok_2Eq:
                    lua_pushboolean(ctx->L, lua_equal(ctx->L,lhs,rhs));
                    break;
                case Tok_BangEq:
                    lua_pushboolean(ctx->L, !lua_equal(ctx->L,lhs,rhs));
                    break;
                default:
                    error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to operand type" );
                    break;
                }
                break;
            case BS_integer:
            case BS_real:
                switch(tok->tok)
                {
                case Tok_2Eq:
                    lua_pushboolean(ctx->L, lua_equal(ctx->L,lhs,rhs));
                    break;
                case Tok_BangEq:
                    lua_pushboolean(ctx->L, !lua_equal(ctx->L,lhs,rhs));
                    break;
                case Tok_Lt:
                    lua_pushboolean(ctx->L, lua_tonumber(ctx->L,lhs) < lua_tonumber(ctx->L,rhs));
                    break;
                case Tok_Leq:
                    lua_pushboolean(ctx->L, lua_tonumber(ctx->L,lhs) <= lua_tonumber(ctx->L,rhs));
                    break;
                case Tok_Gt:
                    lua_pushboolean(ctx->L, lua_tonumber(ctx->L,lhs) > lua_tonumber(ctx->L,rhs));
                    break;
                case Tok_Geq:
                    lua_pushboolean(ctx->L, lua_tonumber(ctx->L,lhs) >= lua_tonumber(ctx->L,rhs));
                    break;
                default:
                    error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to number type" );
                    break;
                }
                break;
            case BS_string:
                switch(tok->tok)
                {
                case Tok_2Eq:
                    lua_pushboolean(ctx->L, lua_equal(ctx->L,lhs,rhs));
                    break;
                case Tok_BangEq:
                    lua_pushboolean(ctx->L, !lua_equal(ctx->L,lhs,rhs));
                    break;
                case Tok_Lt:
                    checkAscii(ctx,lua_tostring(ctx->L,lhs),tok);
                    checkAscii(ctx,lua_tostring(ctx->L,lhs),tok);
                    lua_pushboolean(ctx->L, strcmp(lua_tostring(ctx->L,lhs),lua_tostring(ctx->L,rhs)) < 0);
                    break;
                case Tok_Leq:
                    checkAscii(ctx,lua_tostring(ctx->L,lhs),tok);
                    checkAscii(ctx,lua_tostring(ctx->L,rhs),tok);
                    lua_pushboolean(ctx->L, strcmp(lua_tostring(ctx->L,lhs),lua_tostring(ctx->L,rhs)) <= 0);
                    break;
                case Tok_Gt:
                    checkAscii(ctx,lua_tostring(ctx->L,lhs),tok);
                    checkAscii(ctx,lua_tostring(ctx->L,rhs),tok);
                    lua_pushboolean(ctx->L, strcmp(lua_tostring(ctx->L,lhs),lua_tostring(ctx->L,rhs)) > 0);
                    break;
                case Tok_Geq:
                    checkAscii(ctx,lua_tostring(ctx->L,lhs),tok);
                    checkAscii(ctx,lua_tostring(ctx->L,rhs),tok);
                    lua_pushboolean(ctx->L, strcmp(lua_tostring(ctx->L,lhs),lua_tostring(ctx->L,rhs)) >= 0);
                    break;
                default:
                    error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to string type" );
                    break;
                }
                break;
            case BS_path:
                if( tok->tok == Tok_2Eq || tok->tok == Tok_BangEq )
                {
                    const int eq = lua_equal(ctx->L,lhs,rhs);
                    lua_pushboolean(ctx->L, tok->tok == Tok_2Eq ? eq : !eq );
                }else
                    error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to path type" );
                break;
            default:
                error(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
                break;
            }
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,3);
            lua_getfield(ctx->L,ctx->builtins,"bool");
        }
    }
    BS_END_LUA_FUNC(ctx);
}

static int expression(BSParserContext* ctx, BSScope* scope, int lhsType )
{
    BS_BEGIN_LUA_FUNC(ctx,2); // value, type
    int ret = SimpleExpression(ctx,scope,lhsType);
    BSToken t = peekToken(ctx,1);
    switch(t.tok)
    {
    case Tok_2Eq:
    case Tok_BangEq:
    case Tok_Lt:
    case Tok_Leq:
    case Tok_Gt:
    case Tok_Geq:
    case Tok_in:
        nextToken(ctx);
        SimpleExpression(ctx,scope,lhsType);
        evalRelation(ctx, &t);
        ret = -1;
        break;
    }
    BS_END_LUA_FUNC(ctx);
    return ret;
}

static void block(BSParserContext* ctx, BSScope* scope, BSToken* inLbrace, int pascal);

static void nestedblock(BSParserContext* ctx, BSScope* scope, int _this, BSToken* lbrace, int pascal )
{
    BS_BEGIN_LUA_FUNC(ctx,1);
    lua_createtable(ctx->L,0,0); // create a temporary block definition for the local declarations
    const int blockdecl = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L,BS_BlockDef);
    lua_setfield(ctx->L,blockdecl,"#kind");
    addLocInfo(ctx,lbrace->loc,blockdecl); // TODO: do we also need end of block loc?
    // point to surrounding scope
    lua_pushvalue(ctx->L,scope->table);
    lua_setfield(ctx->L,blockdecl,"#up");

    // the block is associated with the class instance it initializes, so syntax like ".varname" works
    if( _this > 0 )
    {
        lua_pushvalue(ctx->L,_this);
        lua_setfield(ctx->L,blockdecl,"#this");
    }

    lua_createtable(ctx->L,0,0); // create a temporary block instance for the values of local declarations
    const int blockinst = lua_gettop(ctx->L);
    lua_pushvalue(ctx->L,blockinst);
    lua_setfield(ctx->L,blockdecl,"#inst");
    lua_pushvalue(ctx->L,blockdecl);
    lua_setmetatable(ctx->L, blockinst);

    BSScope nested;
    nested.n = 0;
    nested.table = blockdecl;
    block(ctx, &nested, lbrace, pascal);

    lua_pop(ctx->L,1); // blockinst
    BS_END_LUA_FUNC(ctx);
}

static void parampath(BSParserContext* ctx, const char* name, int len)
{
    BS_BEGIN_LUA_FUNC(ctx,2);
    lua_pushlstring(ctx->L,name, len);
    const int path = lua_gettop(ctx->L);
    lua_pushvalue(ctx->L,ctx->module.table);
    const int curmod = lua_gettop(ctx->L);
    int accessible = 1;
    while( !lua_isnil(ctx->L,curmod) )
    {
        lua_getfield(ctx->L,curmod,"#visi");
        const int visi = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1); // visi
        lua_getfield(ctx->L,curmod,"#name");
        if( lua_isnil(ctx->L,-1) )
        {
            lua_pop(ctx->L,1); // name
            break;
        }

        if( visi != BS_Public )
            accessible = 0;

        lua_pushstring(ctx->L,".");
        lua_pushvalue(ctx->L,path);
        lua_concat(ctx->L,3);
        lua_replace(ctx->L,path);

        lua_getfield(ctx->L,curmod,"#owner");
        lua_replace(ctx->L,curmod);
    }
    lua_pop(ctx->L,1); // curmod
    lua_pushinteger(ctx->L,accessible);
    BS_END_LUA_FUNC(ctx);
}

static void vardecl(BSParserContext* ctx, BSScope* scope)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    BSToken t = nextToken(ctx);
    int kind = t.tok;
    if( kind != Tok_var && kind != Tok_let && kind != Tok_param )
        error(ctx, t.loc.row, t.loc.col,"expecting 'var', 'let' or 'param'");
    BSIdentDef id = identdef(ctx, scope);

    if( kind == Tok_param )
    {
        if( id.visi != BS_Private )
            error(ctx, id.loc.row, id.loc.col,"visibility cannot be set for parameters (assumed to be public)");
        id.visi = BS_Public;
    }
    if( kind == Tok_param && scope != &ctx->module )
        error(ctx, t.loc.row, t.loc.col,"parameters are only supported on module level");

    lua_createtable(ctx->L,0,0);
    const int var = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L,BS_VarDecl);
    lua_setfield(ctx->L,var,"#kind");
    addLocInfo(ctx,id.loc,var);
    addXref(ctx, id.loc, var);
    addNumRef(ctx,var);

    switch( kind )
    {
    case Tok_let:
        lua_pushinteger(ctx->L,BS_let);
        break;
    case Tok_var:
        lua_pushinteger(ctx->L,BS_var);
        break;
    case Tok_param:
        lua_pushinteger(ctx->L,BS_param);
        break;
    default:
        assert(0);
    }
    lua_setfield(ctx->L,var,"#rw");

    t = peekToken(ctx,1);
    int explicitType = 0;
    if( t.tok == Tok_Colon )
    {
        nextToken(ctx); // eat ':'

        typeref(ctx,scope);
        lua_pushvalue(ctx->L,-1);
        lua_setfield(ctx->L,var,"#type"); // var points to its type
        explicitType = lua_gettop(ctx->L);
    }else
        lua_pushnil(ctx->L);

    addToScope(ctx, scope, &id, var );

    t = nextToken(ctx);
    if( t.tok == Tok_Lbrace || t.tok == Tok_begin )
    {
        // constructor
        const int pascal = t.tok == Tok_begin;
        if( explicitType == 0 )
            error(ctx, t.loc.row, t.loc.col,"class instance variables require an explicit type" );
        lua_getfield(ctx->L,explicitType,"#kind");
        if( lua_tointeger(ctx->L,-1) != BS_ClassDecl )
            error(ctx, t.loc.row, t.loc.col,"constructors are only supported for class instances" );
        lua_pop(ctx->L,1);
        if( scope != &ctx->module )
            error(ctx, t.loc.row, t.loc.col,"class instance variables only supported on module level");
        if( kind == Tok_param )
            error(ctx, t.loc.row, t.loc.col,"parameter can only be of basic type" );

        lua_createtable(ctx->L,0,0);
        const int classInst = lua_gettop(ctx->L);
        // set the class of the instance
        lua_pushvalue(ctx->L,explicitType);
        lua_setmetatable(ctx->L, classInst);
        lua_pushvalue(ctx->L,var);
        lua_setfield(ctx->L,classInst,"#decl");

        // also make instance accessible by the var decl
        lua_pushvalue(ctx->L,classInst);
        lua_setfield(ctx->L,var,"#inst");

        // set the value of the variable in the scope instance
        lua_pushvalue(ctx->L,scope->table); // +1 scope def
        lua_getfield(ctx->L,-1,"#inst"); // +1 scope inst
        lua_pushlstring(ctx->L,id.name, id.len); // +1
        lua_pushvalue(ctx->L,classInst); // +1
        lua_rawset(ctx->L,-3); // -2
        lua_pop(ctx->L,2); // scope def and inst

        // initialize with default values before block is started
        const int n = lua_objlen(ctx->L, explicitType);
        int i;
        for( i = 1; i <= n; i++ )
        {
            lua_rawgeti(ctx->L,explicitType,i);
            const int decl = lua_gettop(ctx->L);
            lua_getfield(ctx->L,decl,"#name");
            lua_getfield(ctx->L,decl,"#kind");
            int k = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            if( k == BS_FieldDecl )
            {
                lua_getfield(ctx->L,decl,"#type");
                const int type = lua_gettop(ctx->L);
                lua_getfield(ctx->L,-1,"#kind");
                k = lua_tointeger(ctx->L,-1);
                lua_pop(ctx->L,1);
                if( k == BS_ListType )
                {
                    lua_pushvalue(ctx->L, decl+1); // name
                    lua_createtable(ctx->L,0,0);
                    lua_rawset(ctx->L,classInst);
                }else if( k == BS_EnumDecl )
                {
                    lua_pushvalue(ctx->L, decl+1); // name
                    lua_getfield(ctx->L,type,"#default");
                    lua_rawset(ctx->L,classInst);
                }else if( k == BS_BaseType )
                {
                    lua_getfield(ctx->L,type,"#type");
                    const int t = lua_tointeger(ctx->L,-1);
                    lua_pop(ctx->L,1);
                    switch((BSBaseType)t)
                    {
                    case BS_boolean:
                        lua_pushvalue(ctx->L, decl+1); // name
                        lua_pushboolean(ctx->L,0);
                        lua_rawset(ctx->L,classInst);
                        break;
                    case BS_integer:
                        lua_pushvalue(ctx->L, decl+1); // name
                        lua_pushinteger(ctx->L,0);
                        lua_rawset(ctx->L,classInst);
                        break;
                    case BS_real:
                        lua_pushvalue(ctx->L, decl+1); // name
                        lua_pushnumber(ctx->L,0.0);
                        lua_rawset(ctx->L,classInst);
                        break;
                    case BS_string:
                        lua_pushvalue(ctx->L, decl+1); // name
                        lua_pushstring(ctx->L,"");
                        lua_rawset(ctx->L,classInst);
                        break;
                    case BS_path:
                        lua_pushvalue(ctx->L, decl+1); // name
                        lua_pushstring(ctx->L,"."); // RISK
                        lua_rawset(ctx->L,classInst);
                        break;
                    case BS_symbol:
                        lua_pushvalue(ctx->L, decl+1); // name
                        lua_pushstring(ctx->L,"");
                        lua_rawset(ctx->L,classInst);
                        break;
                    case BS_nil:
                        break;
                    }
                }
                lua_pop(ctx->L,1); // type
            }
            lua_pop(ctx->L,2); // decl, name
        }

        nestedblock(ctx,scope, classInst, &t, pascal); // returns block on stack
        if( ctx->fullAst )
        {
            lua_pushvalue(ctx->L,var);
            lua_setfield(ctx->L,-2,"#owner"); // set block.#owner to var
            lua_setfield(ctx->L,var,"#ctr"); // set var.#ctr to block, consuming the stack
        }else
            lua_pop(ctx->L,1);

        if( pascal )
        {
            t = nextToken(ctx);
            if( t.tok != Tok_end )
                error(ctx, id.loc.row, id.loc.col,"expecting 'end'" );
        }
        lua_pop(ctx->L,1); // instance
    }else if( t.tok == Tok_Eq || t.tok == Tok_ColonEq )
    {
        const int ro = expression(ctx,scope,explicitType);
        const int type = lua_gettop(ctx->L);
        if( explicitType != 0 )
        {
            // check type compatibility with explicit type
            if( !sameType(ctx,explicitType,type) && !isSameOrSubclass(ctx,explicitType,type)
                    && !isInEnum(ctx,explicitType,type-1) )
                error(ctx, t.loc.row, t.loc.col,"type of the right hand expression is not compatible" );
        }else
        {
            // use the expression type as the var type
            if( lua_isnil(ctx->L,type) )
                error(ctx, t.loc.row, t.loc.col,"type of the right hand expression cannot be infered" );
            lua_pushvalue(ctx->L,type);
            lua_setfield(ctx->L,var,"#type");
        }

        lua_getfield(ctx->L,type,"#kind");
        const int klt = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);

        if( klt == BS_ClassDecl || klt == BS_ListType )
        {
            if( kind == Tok_param )
                error(ctx, t.loc.row, t.loc.col,"parameter can only be of basic type" );

            if( kind == Tok_var && ro > 0 )
                error(ctx, t.loc.row, t.loc.col,"cannot assign immutable object to var" );
        }
        if( klt != BS_ClassDecl && id.visi == BS_PublicDefault )
            error(ctx, id.loc.row, id.loc.col,"'!' is not applicable here" );

        // store the value to the var instance
        lua_getfield(ctx->L, scope->table, "#inst" );
        const int inst = lua_gettop(ctx->L);
        lua_pushlstring(ctx->L,id.name, id.len);
        lua_pushvalue(ctx->L,type-1);
        lua_rawset(ctx->L,inst);

        if( kind == Tok_param )
        {
            parampath(ctx,id.name,id.len);

            const int accessible = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            const int desig = lua_gettop(ctx->L);
            lua_pushvalue(ctx->L,desig);
            lua_rawget(ctx->L,BS_Params);
            // stack: desig, val or nil
            if( !lua_isnil(ctx->L,-1) )
            {
                if( lua_istable(ctx->L,-1) )
                {
                    lua_rawgeti(ctx->L,-1,1);
                    lua_replace(ctx->L,-2);
                }else if( !accessible )
                    error(ctx, id.loc.row, id.loc.col,
                           "the parameter %s cannot be set because it is not visible from the root directory",
                           lua_tostring(ctx->L,desig));
                // remove the used param from the table
                lua_pushvalue(ctx->L,desig);
                lua_pushnil(ctx->L);
                lua_rawset(ctx->L,BS_Params);
                // stack: desig, val
                const char* val = "";
                switch(lua_type(ctx->L,desig+1))
                {
                case LUA_TNIL:
                    assert(0);
                case LUA_TNUMBER:
                case LUA_TSTRING:
                    val = lua_tostring(ctx->L,desig+1);
                    break;
                case LUA_TBOOLEAN:
                    if( lua_toboolean(ctx->L,desig+1) )
                        val = "true";
                    else
                        val = "false";
                    break;
                default:
                    break;
                }
                uchar len;
                const uint ch = unicode_decode_utf8((const uchar*)val, &len );
                if( len == 0 )
                    error(ctx, id.loc.row, id.loc.col,"passing invalid value to parameter %s: %s",
                           lua_tostring(ctx->L,desig), lua_tostring(ctx->L,desig+1));
                if( unicode_isdigit(ch) || ch == '`' || ch == '$' || ch == '/' || ch == '.'
                        || ch == '\'' || ch == '"' )
                {
                    lua_pushfstring(ctx->L, "parameter '%s': %s", lua_tostring(ctx->L,desig),
                                    lua_tostring(ctx->L,desig+1) );
                    BSLexer* l = bslex_openFromString(lua_tostring(ctx->L,desig+1),lua_objlen(ctx->L,desig+1),lua_tostring(ctx->L,-1));
                    if( l == 0 ) exit(0);
                    BSToken t = bslex_next(l);
                    bslex_free(l);
                    lua_pop(ctx->L,1); // pretty name
                    if( t.tok == Tok_Invalid )
                    {
                        lua_pushnil(ctx->L);
                        lua_error(ctx->L); // bslex_next has already reported
                    }
                    switch(t.tok)
                    {
                    case Tok_integer:
                        lua_getfield(ctx->L,ctx->builtins,"int");
                        lua_pushinteger(ctx->L,lua_tointeger(ctx->L,desig+1));
                        break;
                    case Tok_real:
                        lua_getfield(ctx->L,ctx->builtins,"real");
                        lua_pushnumber(ctx->L,lua_tonumber(ctx->L,desig+1));
                        break;
                    case Tok_path:
                        lua_getfield(ctx->L,ctx->builtins,"path");
                        if( *t.val == '\'')
                            lua_pushlstring(ctx->L,t.val+1,t.len-2); // remove ''
                        else
                            lua_pushvalue(ctx->L,desig+1);
                        break;
                    case Tok_symbol:
                        lua_getfield(ctx->L,ctx->builtins,"symbol");
                        lua_pushstring(ctx->L,lua_tostring(ctx->L,desig+1) + 1); // remove '`'
                        break;
                    case Tok_string:
                        lua_getfield(ctx->L,ctx->builtins,"string");
                        push_unescaped(ctx->L,t.val+1,t.len-2); // remove ""
                        break;
                    default:
                        error(ctx, id.loc.row, id.loc.col,"unexpected parameter value type %s: %s",
                               lua_tostring(ctx->L,desig), lua_tostring(ctx->L,desig+1));
                        break;
                    }
                }else
                {
                    if( strcmp(val,"true") == 0 )
                    {
                        lua_getfield(ctx->L,ctx->builtins,"bool");
                        lua_pushboolean(ctx->L,1);
                    }else if( strcmp(val,"false") == 0 )
                    {
                        lua_getfield(ctx->L,ctx->builtins,"bool");
                        lua_pushboolean(ctx->L,0);
                    }else
                    {
                        lua_getfield(ctx->L,ctx->builtins,"string"); // assume string
                        lua_pushstring(ctx->L,val);
                    }
                }
                // stack: desig, valstr, valtype, val
                lua_getfield(ctx->L,var,"#type");
                // stack: desig, valstr, valtype, val, reftype
                if( !sameType(ctx,-1,-3) && !isInEnum(ctx,-1,-2) )
                    error(ctx, t.loc.row, t.loc.col,"value passed in for parameter '%s' is incompatible",
                           lua_tostring(ctx->L,desig));
                // the param value is ok, assign it
                lua_pushlstring(ctx->L,id.name, id.len);
                lua_pushvalue(ctx->L,desig+3);
                lua_rawset(ctx->L,inst);
                lua_pop(ctx->L,5); // desig, valstr, valtype, val, reftype
            }else
                lua_pop(ctx->L,2); // desig, nil
        }

        lua_pop(ctx->L,3); // the value and type slot returned by expression, and inst

    }else
        error(ctx, t.loc.row, t.loc.col,"expecting '{' or '='" );

    lua_pop(ctx->L,2); // vardecl, explicitType
    BS_END_LUA_FUNC(ctx);
}

static void assignment(BSParserContext* ctx, BSScope* scope, int lro)
{
    BS_BEGIN_LUA_FUNC(ctx,-2);
    // stack: resolved container instance + derefed declaration
    const int lhs = lua_gettop(ctx->L) - 1;

    BSToken t = nextToken(ctx);
    switch(t.tok)
    {
    case Tok_Eq:
    case Tok_ColonEq:
    case Tok_PlusEq:
    case Tok_MinusEq:
    case Tok_StarEq:
        break;
    default:
        error(ctx, t.loc.row, t.loc.col,"expecting '=', '+=', '-=' or '*='" );
        break;
    }

    lua_getfield(ctx->L,lhs+1,"#type");
    const int lt = lua_gettop(ctx->L);

    const int rro = expression(ctx,scope,lt);
    // value, type
    const int rhs = lua_gettop(ctx->L) - 1;

    const int l = isListAndElemType(ctx,lt,rhs+1,lhs,rhs);
    const int sub = isSameOrSubclass(ctx,lt,rhs+1);
    const int same = sameType(ctx,lt,rhs+1);
    const int inenum = isInEnum(ctx,lt,rhs);
    if( !same && !(l == 1 || l == 2) && !sub  && !inenum )
        error(ctx, t.loc.row, t.loc.col,"left and right side are not assignment compatible" );
    if( l == 2 && t.tok == Tok_Eq )
        error(ctx, t.loc.row, t.loc.col,"cannot assign an element to a list; use += instead" );

    lua_getfield(ctx->L,lt,"#kind");
    const int klt = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);

    if( klt == BS_ClassDecl || klt == BS_ListType )
    {
        if( lro == 0 && rro != 0 && ( t.tok == Tok_Eq || t.tok == Tok_ColonEq ) )
            error(ctx, t.loc.row, t.loc.col,"cannot assign immutable object to var" );
        if( lro == 2 && rro == 1 && ( t.tok == Tok_Eq || t.tok == Tok_ColonEq ) )
            error(ctx, t.loc.row, t.loc.col,"cannot assign immutable object to field; use += instead" );
    }

    lua_getfield(ctx->L,lt,"#type");
    const int basetype = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);

    if( !ctx->skipMode )
    {
        switch(t.tok)
        {
        case Tok_Eq:
        case Tok_ColonEq:
            lua_getfield(ctx->L,lhs+1,"#name");
            lua_pushvalue(ctx->L,rhs);
            lua_rawset(ctx->L,lhs);
            break;
        case Tok_PlusEq:
            if( l == 1 || l == 2 )
            {
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_rawget(ctx->L,lhs);
                const int ll = lua_gettop(ctx->L);
                const int nl = lua_objlen(ctx->L,ll);
                const int nr = l == 1 || l == 3 ? lua_objlen(ctx->L,rhs) : 0;
                int i;
                int n = nl;
                for( i = 1; i <= nr; i++ )
                {
                    lua_rawgeti(ctx->L,rhs,i);
                    lua_rawseti(ctx->L,ll,++n);
                }
                if( l == 2 )
                {
                    lua_pushvalue(ctx->L,rhs);
                    lua_rawseti(ctx->L,ll,++n);
                }
                lua_pop(ctx->L,1); // ll
                break;
            }
            switch(basetype)
            {
            case BS_integer:
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_pushvalue(ctx->L,-1);
                lua_rawget(ctx->L,lhs);
                lua_pushinteger(ctx->L, lua_tointeger(ctx->L,-1) + lua_tointeger(ctx->L,rhs));
                lua_replace(ctx->L,-2);
                lua_rawset(ctx->L,lhs);
                break;
            case BS_real:
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_pushvalue(ctx->L,-1);
                lua_rawget(ctx->L,lhs);
                lua_pushnumber(ctx->L, lua_tonumber(ctx->L,-1) + lua_tonumber(ctx->L,rhs));
                lua_replace(ctx->L,-2);
                lua_rawset(ctx->L,lhs);
                break;
            case BS_string:
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_pushvalue(ctx->L,-1);
                lua_rawget(ctx->L,lhs);
                lua_pushvalue(ctx->L,rhs);
                lua_concat(ctx->L,2);
                lua_replace(ctx->L,-2);
                lua_rawset(ctx->L,lhs);
                break;
            case BS_path:
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_pushvalue(ctx->L,-1);
                lua_rawget(ctx->L,lhs);
                addPath(ctx,&t,-1,rhs);
                lua_replace(ctx->L,-2);
                lua_rawset(ctx->L,lhs);
                break;
            default:
                error(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
                break;
            }
            break;
        case Tok_MinusEq:
            if( l == 1 || l == 2 )
            {
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_rawget(ctx->L,lhs);
                const int ll = lua_gettop(ctx->L);
                const int nl = l == 1 || l == 2 ? lua_objlen(ctx->L,ll) : 0;
                const int nr = l == 1 || l == 3 ? lua_objlen(ctx->L,rhs) : 0;
                int i;
                lua_createtable(ctx->L,nr + 1,0); // CHECK: this is barely efficient,
                const int tmp = lua_gettop(ctx->L);
                for( i = 1; i <= nr; i++ )
                {
                    lua_rawgeti(ctx->L,rhs,i);
                    lua_pushvalue(ctx->L,-1);
                    lua_rawset(ctx->L,tmp);
                }
                if( l == 2 )
                {
                    lua_pushvalue(ctx->L,rhs);
                    lua_pushvalue(ctx->L,rhs);
                    lua_rawset(ctx->L,tmp);
                }
                int off = 0;
                for( i = 1; i <= nl; i++ )
                {
                    lua_rawgeti(ctx->L,ll,i);
                    lua_rawget(ctx->L,tmp);
                    const int toRemove = !lua_isnil(ctx->L,-1);
                    lua_pop(ctx->L,1);
                    if( toRemove )
                    {
                        lua_pushnil(ctx->L);
                        lua_rawseti(ctx->L,ll,i);
                        off++;
                    }else if( off )
                    {
                        lua_rawgeti(ctx->L,ll,i);
                        lua_pushnil(ctx->L);
                        lua_rawseti(ctx->L,ll,i);
                        lua_rawseti(ctx->L,ll,i-off);
                    }
                }
                lua_pop(ctx->L,2); // ll, tmp
                break;
            }
            switch(basetype)
            {
            case BS_integer:
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_pushvalue(ctx->L,-1);
                lua_rawget(ctx->L,lhs);
                lua_pushinteger(ctx->L, lua_tointeger(ctx->L,-1) - lua_tointeger(ctx->L,rhs));
                lua_replace(ctx->L,-2);
                lua_rawset(ctx->L,lhs);
                break;
            case BS_real:
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_pushvalue(ctx->L,-1);
                lua_rawget(ctx->L,lhs);
                lua_pushnumber(ctx->L, lua_tonumber(ctx->L,-1) - lua_tonumber(ctx->L,rhs));
                lua_replace(ctx->L,-2);
                lua_rawset(ctx->L,lhs);
                break;
            default:
                error(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
                break;
            }
            break;
        case Tok_StarEq:
            if( l == 1 )
            {
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_rawget(ctx->L,lhs);
                const int ll = lua_gettop(ctx->L);
                const int nl = l == 1 || l == 2 ? lua_objlen(ctx->L,ll) : 0;
                const int nr = l == 1 || l == 3 ? lua_objlen(ctx->L,rhs) : 0;
                lua_createtable(ctx->L,nr + 1,0); // CHECK: this is barely efficient
                const int tmp = lua_gettop(ctx->L);
                int i;
                for( i = 1; i <= nr; i++ )
                {
                    lua_rawgeti(ctx->L,rhs,i);
                    lua_pushvalue(ctx->L,-1);
                    lua_rawset(ctx->L,tmp);
                }
                int off = 0;
                for( i = 1; i <= nl; i++ )
                {
                    lua_rawgeti(ctx->L,ll,i);
                    lua_rawget(ctx->L,tmp);
                    const int toRemove = lua_isnil(ctx->L,-1);
                    lua_pop(ctx->L,1);
                    if( toRemove )
                    {
                        lua_pushnil(ctx->L);
                        lua_rawseti(ctx->L,ll,i);
                        off++;
                    }else if( off )
                    {
                        lua_rawgeti(ctx->L,ll,i);
                        lua_pushnil(ctx->L);
                        lua_rawseti(ctx->L,ll,i);
                        lua_rawseti(ctx->L,ll,i-off);
                    }
                }
                lua_pop(ctx->L,2); // ll, tmp
                break;
            }else if( l == 2 )
            {
                // add element inplace, only if not yet included
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_rawget(ctx->L,lhs);
                const int ll = lua_gettop(ctx->L);
                const int nl = l == 1 || l == 2 ? lua_objlen(ctx->L,ll) : 0;
                // add element, only if not yet included
                int found = 0;
                int i;
                for( i = 1; i <= nl; i++ )
                {
                    lua_rawgeti(ctx->L,ll,i);
                    found = found || lua_equal(ctx->L,-1,rhs);
                    lua_pop(ctx->L,1);
                }
                if( !found )
                {
                    lua_pushvalue(ctx->L,rhs);
                    lua_rawseti(ctx->L,ll, nl+1);
                }
                lua_pop(ctx->L,1); // ll
                break;
            }
            switch(basetype)
            {
            case BS_integer:
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_pushvalue(ctx->L,-1);
                lua_rawget(ctx->L,lhs);
                lua_pushinteger(ctx->L, lua_tointeger(ctx->L,-1) * lua_tointeger(ctx->L,rhs));
                lua_replace(ctx->L,-2);
                lua_rawset(ctx->L,lhs);
                break;
            case BS_real:
                lua_getfield(ctx->L,lhs+1,"#name");
                lua_pushvalue(ctx->L,-1);
                lua_rawget(ctx->L,lhs);
                lua_pushnumber(ctx->L, lua_tonumber(ctx->L,-1) * lua_tonumber(ctx->L,rhs));
                lua_replace(ctx->L,-2);
                lua_rawset(ctx->L,lhs);
                break;
            default:
                error(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
                break;
            }
            break;
        }
    }else
    {
        // still check if valid
        switch(t.tok)
        {
        case Tok_PlusEq:
            if( l == 1 || l == 2  )
                break;
            switch(basetype)
            {
            case BS_integer:
            case BS_real:
            case BS_string:
            case BS_path:
                break;
            default:
                error(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
                break;
            }
            break;
        case Tok_MinusEq:
        case Tok_StarEq:
            if( l == 1 || l == 2  )
                break;
            switch(basetype)
            {
            case BS_integer:
            case BS_real:
                break;
            default:
                error(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
                break;
            }
        }
    }

    lua_pop(ctx->L,1); // lt
    lua_pop(ctx->L,4);

    BS_END_LUA_FUNC(ctx);
}

static void condition(BSParserContext* ctx, BSScope* scope)
{
    BS_BEGIN_LUA_FUNC(ctx,1);
    BSToken t = nextToken(ctx); // eat if
    const int ast = lua_gettop(ctx->L)+1;
    int n = 0;
    if( ctx->fullAst )
    {
        lua_createtable(ctx->L,0,0);
        lua_pushinteger(ctx->L,BS_CondStat);
        lua_setfield(ctx->L,ast,"#kind");
        addLocInfo(ctx,t.loc,ast);
        lua_pushvalue(ctx->L,scope->table);
        lua_setfield(ctx->L,-2,"#owner"); // condition.#owner points to enclosing scope
    }else
        lua_pushnil(ctx->L);
    t = peekToken(ctx,1);
    expression(ctx,scope,0);
    lua_getfield(ctx->L,-1,"#type");
    if( lua_tointeger(ctx->L,-1) != BS_boolean )
        error(ctx, t.loc.row, t.loc.col,"expecting a boolean if expression" );
    lua_pop(ctx->L,1); // type
    int cond = lua_toboolean(ctx->L,-2);
    lua_pop(ctx->L,2); // value, type

    const int skipping = ctx->skipMode;
    if( !skipping )
        ctx->skipMode = !cond;
    t = nextToken(ctx);
    if( t.tok == Tok_then )
    {
        nestedblock(ctx,scope,0,&t,1);
        if( ctx->fullAst )
        {
            lua_pushvalue(ctx->L,ast);
            lua_setfield(ctx->L,-2,"#owner"); // set block.#owner to cond
            lua_rawseti(ctx->L,ast,++n); // add block to cond, consuming the stack
        }else
            lua_pop(ctx->L,1);
        if( !skipping )
            ctx->skipMode = 0;
        t = nextToken(ctx);
        int done = cond;
        while( t.tok == Tok_elsif )
        {
            t = peekToken(ctx,1);
            expression(ctx,scope,0);
            lua_getfield(ctx->L,-1,"#type");
            if( lua_tointeger(ctx->L,-1) != BS_boolean )
                error(ctx, t.loc.row, t.loc.col,"expecting a boolean if expression" );
            lua_pop(ctx->L,1); // type
            cond = lua_toboolean(ctx->L,-2);
            lua_pop(ctx->L,2); // value, type
            t = nextToken(ctx);
            if( t.tok != Tok_then )
                error(ctx, t.loc.row, t.loc.col,"expecting 'then'" );
            if( !skipping )
                ctx->skipMode = !( cond && !done );
            nestedblock(ctx,scope,0,&t,1);
            if( ctx->fullAst )
            {
                lua_pushvalue(ctx->L,ast);
                lua_setfield(ctx->L,-2,"#owner"); // set block.#owner to cond
                lua_rawseti(ctx->L,ast,++n); // add block to cond, consuming the stack
            }else
                lua_pop(ctx->L,1);
            if( !skipping )
                ctx->skipMode = 0;
            t = nextToken(ctx);
            if( cond && !done )
                done = 1;
        }
        if( t.tok == Tok_else )
        {
            if( !skipping )
                ctx->skipMode = done;
            nestedblock(ctx,scope,0,&t,1);
            if( ctx->fullAst )
            {
                lua_pushvalue(ctx->L,ast);
                lua_setfield(ctx->L,-2,"#owner"); // set block.#owner to cond
                lua_rawseti(ctx->L,ast,++n); // add block to cond, consuming the stack
            }else
                lua_pop(ctx->L,1);
            if( !skipping )
                ctx->skipMode = 0;
            t = nextToken(ctx);
        }
        if( t.tok != Tok_end )
            error(ctx, t.loc.row, t.loc.col,"expecting 'end'" );
    }else
    {
        if( t.tok != Tok_Lbrace )
            error(ctx, t.loc.row, t.loc.col,"expecting '{'" );
        nestedblock(ctx,scope,0,&t,0);
        if( ctx->fullAst )
        {
            lua_pushvalue(ctx->L,ast);
            lua_setfield(ctx->L,-2,"#owner"); // set block.#owner to cond
            lua_rawseti(ctx->L,ast,++n); // add block to cond, consuming the stack
        }else
            lua_pop(ctx->L,1);
        if( !skipping )
            ctx->skipMode = 0;
        t = peekToken(ctx,1);
        if( t.tok == Tok_else )
        {
            if( !skipping )
                ctx->skipMode = cond;
            nextToken(ctx); // eat else
            t = peekToken(ctx,1);
            switch( t.tok )
            {
            case Tok_if:
                condition(ctx,scope);
                if( ctx->fullAst )
                    lua_setfield(ctx->L,ast,"#else");
                else
                    lua_pop(ctx->L,1);
                break;
            case Tok_Lbrace:
                nextToken(ctx); // eat lbrace
                nestedblock(ctx,scope,0,&t,0);
                if( ctx->fullAst )
                {
                    lua_pushvalue(ctx->L,ast);
                    lua_setfield(ctx->L,-2,"#owner"); // set block.#owner to cond
                    lua_rawseti(ctx->L,ast,++n); // add block to cond, consuming the stack
                }else
                    lua_pop(ctx->L,1);
                break;
            default:
                error(ctx, t.loc.row, t.loc.col,"expecting 'if' or '{'" );
                break;
            }
            if( !skipping )
                ctx->skipMode = 0;
        }
    }

    BS_END_LUA_FUNC(ctx);
}

static void assigOrCall(BSParserContext* ctx, BSScope* scope)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    BSToken t = peekToken(ctx,1);
    const int lro = resolveInstance(ctx,scope);
    // stack: resolved container instance + derefed declaration

    BSToken t2 = peekToken(ctx,1);
    switch( t2.tok )
    {
    case Tok_Eq:
    case Tok_ColonEq:
    case Tok_PlusEq:
    case Tok_MinusEq:
    case Tok_StarEq:
        if( lro == 1 )
            error(ctx, t.loc.row, t.loc.col,"cannot modify immutable object" );
        assignment(ctx, scope, lro);
        break;
    case Tok_Lpar:
        lua_remove(ctx->L,-2);
        // stack: derefed declaration
        lua_getfield(ctx->L,-1,"#kind");
        if( lua_tointeger(ctx->L,-1) == BS_MacroDef )
        {
            lua_pop(ctx->L,1);
            evalInst(ctx,scope);
            lua_pop(ctx->L,1);
            // stack: --
        }else
        {
            lua_pop(ctx->L,1);
            evalCall(ctx,scope);
            lua_pop(ctx->L,2); // not using return value
        }
        break;
    default:
        error(ctx, t.loc.row, t.loc.col,"looks like an assignment or a call, but next token doesn't fit" );
        break;
    }
    BS_END_LUA_FUNC(ctx);
}

static void block(BSParserContext* ctx, BSScope* scope, BSToken* inLbrace, int pascal)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    BSToken t = peekToken(ctx,1);
    while( !endOfBlock(&t,pascal) && t.tok != Tok_Eof )
    {
        if( ( t.tok == Tok_subdir || t.tok == Tok_submod || t.tok == Tok_submodule ) && &ctx->module == scope )
            submodule(ctx,t.tok == Tok_subdir);
        else if( t.tok == Tok_define && &ctx->module == scope )
            macrodef(ctx);
        else
            switch( t.tok )
            {
            case Tok_var:
            case Tok_let:
            case Tok_param:
                vardecl(ctx, scope);
                break;
            case Tok_type:
                typedecl(ctx, scope);
                break;
            case Tok_if:
                condition(ctx, scope);
                if( ctx->fullAst )
                    lua_rawseti(ctx->L,scope->table,++scope->n);
                else
                    lua_pop(ctx->L,1);
                break;
            case Tok_Hat:
            case Tok_Dot:
            case Tok_ident:
                // start of a designator
                assigOrCall(ctx,scope);
                break;
            default:
                unexpectedToken(ctx, &t,"in block body");
                break;
            }

        t = peekToken(ctx,1);
        if( t.tok == Tok_Semi )
        {
            nextToken(ctx); // eat it
            t = peekToken(ctx,1);
        }
    }
    if( endOfBlock( &t,pascal) )
    {
        if( !inLbrace )
            error(ctx, t.loc.row, t.loc.col,"unexpected '%s'", bslex_tostring(t.tok) );
        else if( !pascal )
            nextToken(ctx); // eat rbrace
    }else if( t.tok == Tok_Eof && inLbrace )
        error(ctx, inLbrace->loc.row, inLbrace->loc.col,"non-terminated block" );
    BS_END_LUA_FUNC(ctx);
}

static const char* calcLabel(const char* path, int level)
{
    const int len = strlen(path);
    if( len == 0 )
        return path; // should never happen
    int i;
    const char* str = path + len;
    while( --str >= path )
    {
        if( *str == '/' )
            level--;
        if( level <= 0 )
        {
            str++;
            return str;
        }
    }
    return str;
}

static int calcLevel(lua_State* L, int module)
{
    int level = 0;
    lua_getfield(L,module,"^");
    while( !lua_isnil(L,-1) )
    {
        level++;
        lua_getfield(L,-1,"^");
    }
    lua_pop(L,1);
    return level;
}

static void moduleerror(BSParserContext* ctx, int module, const char* format, ... )
{
    BSRowCol loc;
    lua_getfield(ctx->L, module, "#row");
    loc.row = lua_tointeger(ctx->L,-1);
    lua_getfield(ctx->L, module, "#col");
    loc.col = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,2);

    lua_getfield(ctx->L,module,"^");
    const char* path = 0;
    if( lua_istable(ctx->L,-1) )
    {
        lua_getfield(ctx->L,-1,"#file");
        path = bs_denormalize_path(lua_tostring(ctx->L,-1));
        lua_pop(ctx->L,2);
    }else
    {
        lua_getfield(ctx->L,module,"#file");
        path = bs_denormalize_path(lua_tostring(ctx->L,-1));
        loc.row = 0;
        lua_pop(ctx->L,2);
    }
    va_list args;
    va_start(args, format);
    ctx->logger(BS_Error, ctx->loggerData,path,loc, format, args );
    va_end(args);
    lua_pushnil(ctx->L);
    lua_error(ctx->L);
}

typedef struct BSPresetLogger {
    BSLogger logger;
    void* data;
} BSPresetLogger;

int bs_parse(lua_State* L)
{
    if( lua_isnil(L,BS_Params) )
    {
        lua_createtable(L,0,0);
        lua_replace(L,BS_Params);
    }
    assert( lua_istable(L,BS_NewModule) );

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getglobal(L,"#haveXref");
    const int haveXref = !lua_isnil(L,-1);
    lua_pop(L,1);

    if( haveXref )
        lua_getglobal(L,"#xref");
    else
        lua_pushnil(L);
    const int xref = lua_gettop(L);

    lua_getglobal(L,"#haveNumRefs");
    const int haveNumRefs = !lua_isnil(L,-1);
    lua_pop(L,1);

    if( haveNumRefs )
        lua_getglobal(L,"#refs");
    else
        lua_pushnil(L);
    const int numRefs = lua_gettop(L);


    lua_createtable(L,0,0); // module instance
    lua_pushvalue(L,-1);
    lua_setfield(L,BS_NewModule,"#inst");
    lua_pushvalue(L,BS_NewModule);
    lua_setmetatable(L, -2); // point to declaration of module instance
    lua_pop(L,1);

    BSParserContext ctx;
    memset(&ctx,0,sizeof(BSParserContext));
    ctx.module.table = BS_NewModule;
    ctx.module.n = 0;
    ctx.L = L;
    ctx.dirpath = lua_tostring(L,BS_PathToSourceRoot);
    ctx.builtins = builtins;
    ctx.label = calcLabel(ctx.dirpath,calcLevel(L,BS_NewModule)+1);
    lua_getglobal(L,"#haveLocInfo");
    ctx.locInfo = !lua_isnil(L,-1);
    lua_pop(L,1);
    lua_getglobal(L,"#haveFullAst");
    ctx.fullAst = !lua_isnil(L,-1);
    lua_pop(L,1);

    if( haveXref )
        ctx.xref = xref;
    if( haveNumRefs )
        ctx.numRefs = numRefs;

    lua_getglobal(L,"#logger");
    BSPresetLogger* psl = (BSPresetLogger*)lua_touserdata(L,-1);
    if( psl == 0 || psl->logger == 0 )
    {
        ctx.logger = bs_defaultLogger;
        ctx.loggerData = 0;
    }else
    {
        ctx.logger = psl->logger;
        ctx.loggerData = psl->data;
    }
    lua_pop(L,1);

    if( strncmp(ctx.dirpath,"//",2) != 0 )
        moduleerror(&ctx,BS_NewModule,"expecting absolute, normalized directory path: %s", ctx.dirpath );

    lua_pushstring(L,ctx.label);
    lua_setfield(L,BS_NewModule, "#label");
    // BS_BEGIN_LUA_FUNC(&ctx,1); cannot use this here because module was created above already
    const int $stack = lua_gettop(L)+1;

    lua_pushvalue(L,BS_PathToSourceRoot);
    lua_setfield(L,BS_NewModule,"#dir");
    lua_pushvalue(L,BS_PathToSourceRoot);
    lua_pushstring(L,"/");
    lua_pushstring(L,"BUSY");
    lua_concat(L,3);
    if( !bs_exists( lua_tostring(L,-1) ) )
    {
        lua_getfield(L,BS_NewModule,"#altmod");
        if( lua_isnil(L,-1) )
            lua_pop(L,1);
        else if( !bs_exists( lua_tostring(L,-1) ) )
            moduleerror(&ctx,BS_NewModule,"neither can find '%s' nor alternative path '%s'",
                        lua_tostring(L,-2), lua_tostring(L,-1) );
        else
            lua_replace(L,-2);
        lua_pushboolean(L,1);
        lua_setfield(L,BS_NewModule,"#dummy");
        // message(&ctx,"# analyzing %s on behalf of %s\n", lua_tostring(L,-1), ctx.dirpath);
    }else
        message(&ctx,"# analyzing %s", lua_tostring(L,-1));
    ctx.filepath = lua_tostring(L,-1);
    lua_setfield(L,BS_NewModule,"#file");

    if( haveXref )
    {
        lua_pushstring(ctx.L,bs_denormalize_path(ctx.filepath));
        lua_rawget(L,ctx.xref);
        if( !lua_istable(L,-1) )
        {
            lua_pop(L,1); // nil
            lua_pushstring(ctx.L,bs_denormalize_path(ctx.filepath));
            lua_createtable(L,0,0);
            lua_rawset(L,ctx.xref);
        }else
            lua_pop(L,1); // list
    }

    addNumRef(&ctx,BS_NewModule);
    if( ctx.numRefs )
    {
        // also add path->module to #refs for each module
        // TODO: a path can point to more than one module
        lua_pushstring(ctx.L,bs_denormalize_path(ctx.filepath));
        lua_pushvalue(ctx.L,BS_NewModule);
        lua_rawset(ctx.L,ctx.numRefs);
    }

    ctx.lex = bslex_createhilex(bs_denormalize_path(ctx.filepath), labelOrFilepath(&ctx) );
    if( ctx.lex == 0 )
    {
        lua_pop(L,1);
        lua_pushnil(L);
        lua_error(L);
    }

    block(&ctx,&ctx.module,0,0);
    lua_pushvalue(L,BS_NewModule);

    bslex_freehilex(ctx.lex);

    BS_END_LUA_FUNC(&ctx);
    return 1;
}

void bs_preset_logger(lua_State* L, BSLogger l, void* data)
{
    BSPresetLogger* ps = (BSPresetLogger*)lua_newuserdata(L,sizeof(BSPresetLogger));
    ps->logger = l;
    ps->data = data;
    lua_setglobal(L,"#logger");
}

int bs_dump(lua_State *L)
{
    BSPresetLogger l;
    l.data = 0;
    l.logger = bs_defaultLogger;
    lua_getglobal(L,"#logger");
    BSPresetLogger* psl = (BSPresetLogger*)lua_touserdata(L,-1);
    if( psl == 0 || psl->logger == 0 )
        l = *psl;
    lua_pop(L,1);
    if( lua_isstring(L,2) )
        dumpimp(L,1,l.logger,l.data,lua_tostring(L,2));
    else
        dumpimp(L,1,l.logger,l.data, 0);
    return 0;
}

void bs_dump2(lua_State *L, const char* title, int index )
{
    BSPresetLogger l;
    l.data = 0;
    l.logger = bs_defaultLogger;
    lua_getglobal(L,"#logger");
    BSPresetLogger* psl = (BSPresetLogger*)lua_touserdata(L,-1);
    if( psl == 0 || psl->logger == 0 )
        l = *psl;
    lua_pop(L,1);
    dumpimp(L,index,l.logger,l.data,title);
}



