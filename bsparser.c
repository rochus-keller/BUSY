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
    BSLexer* lex;
    BSScope module;
    const char* dirpath;  // normalized absolute path to current dir
    const char* label; // pointer to internal of dirpath to display in error messages
    const char* filepath; // normalized absolute path to BUSY in current dir
    unsigned builtins : 8;
    unsigned skipMode : 1; // when on, read over tokens without executing
    lua_State* L;
} BSParserContext;

static BSToken nextToken(BSParserContext* ctx)
{
    return bslex_next(ctx->lex);
}

static BSToken peekToken(BSParserContext* ctx, int off ) // off = 1..
{
    return bslex_peek(ctx->lex,off);
}

typedef struct BSIdentDef {
    const char* name;
    int len;
    int visi; // BSVisibility
    BSRowCol loc;
} BSIdentDef;

static void error2(BSParserContext* ctx, int row, int col, const char* format, ... )
{
    fprintf(stderr,"%s:%d:%d:ERR: ", ctx->label, row, col);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr,"\n");
    fflush(stderr);
    lua_pushnil(ctx->L);
    lua_error(ctx->L);
}

static void warning(BSParserContext* ctx, int row, int col, const char* format, ... )
{
    fprintf(stderr,"%s:%d:%d:WRN: ", ctx->label, row, col);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr,"\n");
    fflush(stderr);
}

static void error3(BSParserContext* ctx, BSToken* t, const char* format, ... )
{
    fprintf(stderr,"%s:%d:%d:ERR: ", ctx->label, t->loc.row, t->loc.col);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr,"\n");
    fflush(stderr);
    lua_pushnil(ctx->L);
    lua_error(ctx->L);
}

static void unexpectedToken(BSParserContext* ctx, BSToken* t)
{
    error3(ctx,t,"unexpected token: %s", bslex_tostring(t->tok) );
}

#define BS_BEGIN_LUA_FUNC(ctx,diff) const int $stack = lua_gettop((ctx)->L) + diff
#define BS_END_LUA_FUNC(ctx) int $end = lua_gettop((ctx)->L); assert($stack == $end)

static void dumpimp(lua_State* L, int index, int nl)
{
    if( index <= 0 )
        index += lua_gettop(L) + 1;
    switch( lua_type(L,index) )
    {
    case LUA_TNIL:
        fprintf(stdout,"nil");
        break;
    case LUA_TBOOLEAN:
        fprintf(stdout,"bool %d",lua_toboolean(L,index));
        break;
    case LUA_TNUMBER:
        if( ( (int)lua_tonumber(L,index) - lua_tonumber(L,index) ) == 0.0 )
            fprintf(stdout,"int %d",lua_tointeger(L,index));
        else
            fprintf(stdout,"number %f",lua_tonumber(L,index));
        break;
    case LUA_TSTRING:
        fprintf(stdout,"string \"%s\"",lua_tostring(L,index));
        break;
    case LUA_TTABLE:
        fprintf(stdout,"*** table: %p", lua_topointer(L,index));
        if( lua_getmetatable(L,index) )
        {
            fprintf(stdout,"\n  metatable %p", lua_topointer(L,-1));
            lua_pop(L,1);
        }
        lua_pushnil(L);  /* first key */
        while (lua_next(L, index) != 0) {
            fprintf(stdout,"\n  ");
            if( lua_type(L,-2) == LUA_TTABLE )
                fprintf(stdout,"table %p", lua_topointer(L,-2));
            else
                dumpimp(L,-2,0);
            fprintf(stdout," = ");
            if( lua_type(L,-1) == LUA_TTABLE )
                fprintf(stdout,"table %p", lua_topointer(L,-1));
            else
                dumpimp(L,-1,0);
            lua_pop(L, 1);
        }
        break;
    case LUA_TFUNCTION:
        fprintf(stdout,"function %p",lua_topointer(L,index));
        break;
    default:
        fprintf(stdout,"<lua value>");
        break;
    }
    if( nl )
    {
        fprintf(stdout,"\n");
        fflush(stdout);
    }
}

int bs_dump(lua_State *L)
{
    if( lua_isstring(L,2) )
        fprintf(stdout,"%s: ", lua_tostring(L,2) );
    dumpimp(L,1,1);
    return 0;
}

void bs_dump2(lua_State *L, const char* title, int index )
{
    fprintf(stdout,"%s: ", title);
    dumpimp(L,index,1);
}

static void dump(BSParserContext* ctx, int index)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    dumpimp(ctx->L,index,1);
    BS_END_LUA_FUNC(ctx);
}

static void dump2(BSParserContext* ctx, const char* title, int index)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    fprintf(stdout,"%s: ", title);
    dumpimp(ctx->L,index,1);
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
        error2(ctx,id->loc.row, id->loc.col,"name is not unique in scope: '%s'", name );
}

static BSIdentDef identdef(BSParserContext* ctx, BSScope* scope)
{
    BSToken t = nextToken(ctx);
    if( t.tok != Tok_ident )
        error2(ctx, t.loc.row, t.loc.col , "expecting an ident");
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

static void subdirectory(BSParserContext* ctx)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    nextToken(ctx); // keyword
    BSIdentDef id = identdef(ctx,&ctx->module);
    const char* subdirname = id.name;
    int len = id.len;
    if( id.visi == BS_PublicDefault )
        error2(ctx, id.loc.row, id.loc.col,"'!' is not applicable here" );
    BSToken t = peekToken(ctx,1);
    if( t.tok == Tok_Eq || t.tok == Tok_ColonEq )
    {
        nextToken(ctx); // eat '='
        t = nextToken(ctx);
        if( t.tok == Tok_path || t.tok == Tok_ident )
        {
            subdirname = t.val;
            len = t.len;
            if( t.tok == Tok_path )
            {
                // check path is relative and only one level
                if( *subdirname == '\'' )
                {
                    subdirname++;
                    len -= 2;
                }
                if( strncmp(subdirname,"//",2) == 0 || strncmp(subdirname,"..",2) == 0)
                    error2(ctx, t.loc.row, t.loc.col,"this path is not supported here" );
                if( strncmp(subdirname,".",1) == 0 )
                {
                    subdirname += 2;
                    len -= 2;
                }
                int i;
                for( i = 0; i < len; i++ )
                {
                    if( subdirname[i] == '/' )
                        error2(ctx, t.loc.row, t.loc.col,"expecting an immediate subdirectory" );
                }
            }
        }else
            error2(ctx, t.loc.row, t.loc.col,"expecting a path or an ident" );
    }

    lua_createtable(ctx->L,0,0); // module definition
    const int module = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L, BS_ModuleDef);
    lua_setfield(ctx->L,-2,"#kind");
    lua_pushvalue(ctx->L,ctx->module.table);
    lua_setfield(ctx->L,-2,"^"); // set reference to outer scope
    addToScope(ctx, &ctx->module, &id, module ); // the outer module directly references this module, no adapter

    lua_getfield(ctx->L,ctx->module.table,"#rdir");
    lua_pushstring(ctx->L,"/");
    lua_pushlstring(ctx->L,subdirname,len);
    lua_pushvalue(ctx->L,-1);
    lua_setfield(ctx->L,module,"#dirname");
    lua_concat(ctx->L,3);
    lua_setfield(ctx->L,module,"#rdir");

    lua_pushcfunction(ctx->L, bs_parse);

    lua_pushstring(ctx->L,ctx->dirpath);
    lua_pushstring(ctx->L,"/");
    lua_pushlstring(ctx->L,subdirname,len);
    lua_concat(ctx->L,3);

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
            error2(ctx, t.loc.row, t.loc.col,"designator cannot start with '.' here" );
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
        error2(ctx, t.loc.row, t.loc.col, "designator must start with a '^', '.' or identifier" );
        break;
    }
    // here we have the first scope on stack from where we resolve the ident
    if( t.tok != Tok_ident )
        error2(ctx, t.loc.row, t.loc.col, "expecting an identifier here" );

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
                    error2(ctx, t.loc.row, t.loc.col, "the identifier is not visible from here" );
                else
                {
                    // stack: module def, decl
                    lua_getfield(ctx->L,-2,"#inst");
                    // stack: module def, decl, inst
                    lua_replace(ctx->L,-3); // replace the module def by its instance
                    // result stack: module inst, decl
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
                }
            }
        }
    }
    if( lua_isnil(ctx->L,-1) )
        error2(ctx, t.loc.row, t.loc.col,
               "identifier doesn't reference a declaration; check spelling and declaration order" );
    if( method != Field )
    {
        lua_getfield(ctx->L,-1,"#ro");
        const int ro = lua_toboolean(ctx->L,-1);
        lua_pop(ctx->L,1);
        if( ro )
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
            error2(ctx, t.loc.row, t.loc.col, "cannot dereference a type declaration or procedure" );
            break;
        case BS_FieldDecl:
        case BS_VarDecl:
            // we want to dereference a var or field, so we need the class type of it
            // stack: container instance, derefed decl, new instance
            lua_getfield(ctx->L,-2,"#type");
            // stack: old instance, derefed decl, new instance, class decl

            lua_getfield(ctx->L,-1,"#kind");
            if( lua_tointeger(ctx->L,-1) != BS_ClassDecl )
                error2(ctx, t.loc.row, t.loc.col, "can only dereference fields or variables of class type" );
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
            error2(ctx, t.loc.row, t.loc.col, "dereferencing a nil value" );

        t = nextToken(ctx); // ident
        if( t.tok != Tok_ident )
            error2(ctx, t.loc.row, t.loc.col, "expecting an ident" );

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
            error2(ctx, t.loc.row, t.loc.col, "unknown identifier" );

        lua_replace(ctx->L, -3);
        // stack: new instance, derefed decl

        lua_getfield(ctx->L,-1,"#kind");
        kind = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);

        switch( kind )
        {
        case BS_ModuleDef:
        case BS_VarDecl:
            lua_getfield(ctx->L,-1,"#visi");
            if( lua_tointeger(ctx->L,-1) < BS_Public )
                error2(ctx, t.loc.row, t.loc.col, "the identifier is not visible from here" );
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
    t = nextToken(ctx);
    int n = 0;
    while( t.tok != Tok_Rpar )
    {
        if( t.tok == Tok_Eof )
            error2(ctx, lpar.loc.row, lpar.loc.col,"non-terminated enum type declaration" );
        if( t.tok == Tok_symbol )
        {
            lua_pushlstring(ctx->L, t.val+1, t.len-1); // remove leading `
            const int name = lua_gettop(ctx->L);
            lua_getfield(ctx->L,decl,lua_tostring(ctx->L,name));
            // check for duplicates
            if( !lua_isnil(ctx->L,-1) )
                error2(ctx, t.loc.row, t.loc.col,"duplicate field name" );
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
            error2(ctx, t.loc.row, t.loc.col,"%d:%d: expecting a symbol or ')'" );

        t = nextToken(ctx);
    }
    if( n == 0 )
        error2(ctx, t.loc.row, t.loc.col,"enum type cannot be empty" );

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
        error2(ctx, t.loc.row, t.loc.col,"designator doesn't point to a valid type" );

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
    t = peekToken(ctx,1);
    int n = 0;
    if( t.tok == Tok_Lpar )
    {
        nextToken(ctx); // eat it
        resolveDecl(ctx, scope);
        assert( !lua_isnil(ctx->L,-1) );
        lua_getfield(ctx->L,-1,"#kind");
        if( lua_tointeger(ctx->L,-1) != BS_ClassDecl )
            error2(ctx, t.loc.row, t.loc.col,"invalid superclass" );
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
            error2(ctx, t.loc.row, t.loc.col ,"expecting ')'");
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
            error2(ctx, cls.loc.row, cls.loc.col,"non-terminated class declaration" );
        if( t.tok != Tok_ident )
            error2(ctx, t.loc.row, t.loc.col,"expecting identifier" );

        lua_pushlstring(ctx->L,t.val,t.len);
        const int name = lua_gettop(ctx->L);

        lua_getfield(ctx->L,clsDecl,lua_tostring(ctx->L,name));
        // check for duplicates
        if( !lua_isnil(ctx->L,-1) )
            error2(ctx, t.loc.row, t.loc.col,"duplicate field name" );
        else
            lua_pop(ctx->L,1);

        lua_createtable(ctx->L,0,0);
        const int field = lua_gettop(ctx->L);
        // the following does the same as addToScope, but for the class
        lua_pushinteger(ctx->L,BS_FieldDecl);
        lua_setfield(ctx->L,field,"#kind");

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
            error2(ctx, t.loc.row, t.loc.col,"expecting ':'" );

        t = peekToken(ctx,1);
        typeref(ctx,scope);

        lua_getfield(ctx->L,-1,"#kind");
        const int kind = lua_tointeger(ctx->L, -1);
        lua_pop(ctx->L,1);
        if( kind == BS_ClassDecl )
            error2(ctx, t.loc.row, t.loc.col,"fields cannot be of class type; use a list instead" );
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
        error2(ctx, id.loc.row, id.loc.col,"'!' is not applicable here" );
    BSToken t = nextToken(ctx);
    if( t.tok != Tok_Eq )
        error2(ctx, t.loc.row, t.loc.col,"expecting '='" );
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
        error2(ctx, t.loc.row, t.loc.col,"invalid type declaration" );
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

static int sameType( BSParserContext* ctx, int a, int b )
{
    const int top = lua_gettop(ctx->L);
    if( a <= 0 )
        a += top + 1;
    if( b <= 0 )
        b += top + 1;
    if( lua_equal(ctx->L,a,b) )
        return 1;
    if( !lua_istable(ctx->L,a) || !lua_istable(ctx->L,b) )
        return 0;
    lua_getfield(ctx->L,a,"#kind");
    const int ka = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    lua_getfield(ctx->L,b,"#kind");
    const int kb = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    if( ka != kb )
        return 0;
    if( ka == BS_ClassDecl || kb == BS_ClassDecl || ka == BS_EnumDecl || kb == BS_EnumDecl )
        return 0; // we already checked whether table a and b ar the same
    if( ka == BS_BaseType && kb == BS_BaseType )
    {
        lua_getfield(ctx->L,a,"#type");
        const int ta = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        lua_getfield(ctx->L,b,"#type");
        const int tb = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        return ta == tb;
    }
    if( ka == BS_ListType && kb == BS_ListType )
    {
        lua_getfield(ctx->L,a,"#type");
        lua_getfield(ctx->L,b,"#type");
        const int res = sameType(ctx,-1,-2);
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
        error2(ctx, row, col,"expecting two arguments" );
    const int arg1 = lua_gettop(ctx->L) - 4 + 1;
    const int arg2 = arg1 + 2;
    lua_getfield(ctx->L,arg1+1,"#kind");
    lua_getfield(ctx->L,arg2+1,"#kind");
    if( lua_tointeger(ctx->L,-1) != BS_ListType || lua_tointeger(ctx->L,-2) != BS_ListType )
        error2(ctx, row, col,"expecting two arguments of list type");
    lua_pop(ctx->L,2);
    lua_getfield(ctx->L,arg1+1,"#type");
    lua_getfield(ctx->L,arg2+1,"#type");
    if( !sameType(ctx,-2,-1) )
        error2(ctx, row, col,"expecting two arguments of same list type" );
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
        error2(ctx, row, col,"expecting two arguments" );
    const int arg1 = lua_gettop(ctx->L) - 4 + 1;
    const int arg2 = arg1 + 2;
    lua_getfield(ctx->L,arg1+1,"#kind");
    lua_getfield(ctx->L,arg2+1,"#kind");
    if( lua_tointeger(ctx->L,-1) != BS_ListType || lua_tointeger(ctx->L,-2) != BS_ListType )
        error2(ctx, row, col,"expecting two arguments of list type" );
    lua_pop(ctx->L,2);
    lua_getfield(ctx->L,arg1+1,"#type");
    lua_getfield(ctx->L,arg2+1,"#type");
    if( !sameType(ctx,-2,-1) )
        error2(ctx, row, col,"expecting two arguments of same list type" );
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
                error2(ctx, row, col,"invalid argument type" );
            lua_pop(ctx->L,1);
            lua_getfield(ctx->L,-1,"#dir");
            lua_replace(ctx->L,-2);
        }else
        {
            lua_getfield(ctx->L,-1,"#kind");
            lua_getfield(ctx->L,-2,"#type");
            if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_path )
                error2(ctx, row, col,"expecting argument of type path" );
            lua_pop(ctx->L,2);
            if( *lua_tostring(ctx->L,-2) == '/' )
                lua_pushvalue(ctx->L,-2);
            else
            {
                lua_getfield(ctx->L,ctx->module.table,"#dir");
                if( bs_add_path(ctx->L,-1,-3) != 0 )
                    error2(ctx, row, col,"cannot convert this path" );
            }
            lua_replace(ctx->L,-2);
        }
        lua_getfield(ctx->L,ctx->builtins, "path");
    }else if( n == 2 )
    {
        lua_getfield(ctx->L,-1,"#kind");
        lua_getfield(ctx->L,-2,"#type");
        if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_path )
            error2(ctx, row, col,"expecting second argument of type path" );
        lua_pop(ctx->L,2);
        if( !lua_isnil(ctx->L,-3) || !lua_istable(ctx->L,-4) )
            error2(ctx, row, col,"expecting first argument of module type" );
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
                error2(ctx, row, col,"cannot convert this path" );
            // v1, t1, v2, t2, dir, path
            lua_replace(ctx->L,-2);
            // v1, t1, v2, t2, path
        }
        lua_getfield(ctx->L,ctx->builtins, "path");
        // dir, t
    }else
        error2(ctx, row, col,"expecting zero, one or two arguments" );
    BS_END_LUA_FUNC(ctx);
}

static void readstring(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error2(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_path )
        error2(ctx, row, col,"expecting one argument of type path" );
    lua_pop(ctx->L,2);

    // stack: value, type
    if( *lua_tostring(ctx->L,-2) != '/' )
    {
        lua_getfield(ctx->L,ctx->module.table,"#dir");
        if( bs_add_path(ctx->L,-1,-3) != 0 )
            error2(ctx, row, col,"cannot convert this path" );
        // stack: rel, type, field, abs
        lua_replace(ctx->L,-4);
        lua_pop(ctx->L,1);
    }

    FILE* f = bs_fopen(bs_denormalize_path(lua_tostring(ctx->L,-2)),"r");
    if( f == NULL )
        error2(ctx, row, col,"cannot open file for reading: %s", lua_tostring(ctx->L,-2) );
    fseek(f, 0L, SEEK_END);
    int sz = ftell(f);
    if( sz < 0 )
        error2(ctx, row, col,"cannot determine file size: %s", lua_tostring(ctx->L,-2) );
    rewind(f);
    if( sz > 16000 )
        error2(ctx, row, col,"file is too big to be read: %s", lua_tostring(ctx->L,-2) );
    char* tmp1 = (char*) malloc(sz+1);
    char* tmp2 = (char*) malloc(2*sz+1);
    if( tmp1 == NULL || tmp2 == NULL )
        error2(ctx, row, col,"not enough memory to read file: %s", lua_tostring(ctx->L,-2) );
    if( fread(tmp1,1,sz,f) != (size_t)sz )
    {
        free(tmp1);
        free(tmp2);
        error2(ctx, row, col,"error reading file: %s", lua_tostring(ctx->L,-2) );
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
            error2(ctx, row, col,"invalid utf-8 format: %s", lua_tostring(ctx->L,-2) );
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
                error2(ctx, row, col,"invalid argument type" );
            lua_pop(ctx->L,1);
            lua_getfield(ctx->L,-1,"#rdir");
            lua_replace(ctx->L,-2);
            lua_getfield(ctx->L,ctx->builtins, "path");
        }else
            error2(ctx, row, col,"invalid argument type" );
    }else
        error2(ctx, row, col,"expecting zero or one arguments" );
    BS_END_LUA_FUNC(ctx);
}

static void toint(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error2(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_real )
        error2(ctx, row, col,"expecting one argument of type real" );
    lua_pop(ctx->L,2);
    lua_pushinteger(ctx->L, lua_tonumber(ctx->L,-2));
    lua_getfield(ctx->L,ctx->builtins, "int");
    BS_END_LUA_FUNC(ctx);
}

static void toreal(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error2(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_integer )
        error2(ctx, row, col,"expecting one argument of type integer" );
    lua_pop(ctx->L,2);
    lua_pushnumber(ctx->L, lua_tointeger(ctx->L,-2));
    lua_getfield(ctx->L,ctx->builtins, "real");
    BS_END_LUA_FUNC(ctx);
}

static void tostring(BSParserContext* ctx, int n, int row, int col)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n != 1 )
        error2(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    const int k = lua_tointeger(ctx->L,-2);
    if( k != BS_BaseType && k != BS_EnumDecl )
        error2(ctx, row, col,"expecting one argument of a base type" );
    const int type = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,2);
    switch(type)
    {
    case BS_boolean:
        lua_pushstring(ctx->L, lua_toboolean(ctx->L,-2) ? "true" : "false" );
        break;
    case BS_path:
        lua_pushstring(ctx->L, bs_denormalize_path(lua_tostring(ctx->L,-2)) );
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
        error2(ctx, row, col,"expecting one argument" );
    lua_getfield(ctx->L,-1,"#kind");
    lua_getfield(ctx->L,-2,"#type");
    if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_string )
        error2(ctx, row, col,"expecting one argument of string type" );
    lua_pop(ctx->L,2);
    const char* str = lua_tostring(ctx->L,-2);

    BSPathStatus res = bs_normalize_path2(str);
    switch(res)
    {
    case BS_OK:
        break;
    case BS_NotSupported:
        error2(ctx, row, col,"this path format is not supported" );
        break;
    case BS_InvalidFormat:
        error2(ctx, row, col,"this path format is invalid" );
        break;
    case BS_OutOfSpace:
        error2(ctx, row, col,"this path is too long to be handled" );
        break;
    case BS_NOP:
        assert(0);
        break; // never happens
    }

    lua_pushstring(ctx->L,bs_global_buffer());
    lua_getfield(ctx->L,ctx->builtins, "path");
    BS_END_LUA_FUNC(ctx);
}

// kind = 0: message; = 1: warning; = 2: error
static void print(BSParserContext* ctx, int n, int row, int col, int kind)
{
    BS_BEGIN_LUA_FUNC(ctx,2); // out: value, type
    if( n < 1 )
        error2(ctx, row, col,"expecting at least one argument" );
    int i;
    const int first = lua_gettop(ctx->L) - 2 * n + 1;
    for( i = 0; i < n; i++ )
    {
        const int value = first+2*i;
        lua_getfield(ctx->L,value+1,"#kind");
        lua_getfield(ctx->L,value+1,"#type");
        if( lua_tointeger(ctx->L,-2) != BS_BaseType || lua_tointeger(ctx->L,-1) != BS_string )
            error2(ctx, row, col,"expecting one or more arguments of type string" );
        lua_pop(ctx->L,2);
        lua_pushvalue(ctx->L,value);
    }
    lua_concat(ctx->L,n);
    if( !ctx->skipMode )
    {
        switch( kind )
        {
        case 2:
            lua_error(ctx->L);
            break;
        case 1:
            fprintf(stderr,"%s:%d:%d:WARNING: %s\n",ctx->label, row, col, lua_tostring(ctx->L,-1) );
            break;
        default:
            fprintf(stdout,"%s:%d:%d: %s\n", ctx->label, row, col, lua_tostring(ctx->L,-1) );
            break;
        }
        fflush(stdout);
    }

    lua_pop(ctx->L,1);

    lua_pushnil(ctx->L); // no return value and type
    lua_pushnil(ctx->L);
    BS_END_LUA_FUNC(ctx);
}

static int expression(BSParserContext* ctx, BSScope* scope, int lhsType);

static void evalCall(BSParserContext* ctx, BSScope* scope)
{
    // in: proc declaration
    BS_BEGIN_LUA_FUNC(ctx,1); // out: value, type
    const int proc = lua_gettop(ctx->L); // becomes value
    lua_pushnil(ctx->L); // becomes type

    BSToken t = nextToken(ctx);
    if( t.tok != Tok_Lpar)
        error2(ctx, t.loc.row, t.loc.col,"expecting '('" );
    BSToken lpar = t;

    lua_getfield(ctx->L,proc,"#kind");
    const int kind = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    if( kind != BS_ProcDef )
        error2(ctx, lpar.loc.row, lpar.loc.col,"the designated object is not callable" );

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
        error2(ctx, lpar.loc.row, lpar.loc.col,"argument list not terminated" );

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
                error2(ctx, lpar.loc.row, lpar.loc.col,"expecting one or two arguments" );
            if( n == 2 )
            {
                fprintf(stdout,"%s: ", lua_tostring(ctx->L,lua_gettop(ctx->L)-2+1));
                dump(ctx,lua_gettop(ctx->L)-4+1);
            }else
                dump(ctx,lua_gettop(ctx->L)-2+1);
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
    default:
        error2(ctx, lpar.loc.row, lpar.loc.col,"procedure not yet implemented" );
    }

    lua_replace(ctx->L,proc+1);
    lua_replace(ctx->L,proc);
    lua_pop(ctx->L,n*2);

    BS_END_LUA_FUNC(ctx);
}


// returns 0: no list or incompatible; 1: both list; 2: left list; 3: right list
static int isListAndElemType( BSParserContext* ctx, int lhs, int rhs )
{
    // lhs is list and rhs is list or element or vice versa; lhs and rhs point to types
    const int top = lua_gettop(ctx->L);
    if( lhs <= 0 )
        lhs += top + 1;
    if( rhs <= 0 )
        rhs += top + 1;
    if( !lua_istable(ctx->L,lhs) || !lua_istable(ctx->L,rhs) )
        return 0;
    lua_getfield(ctx->L,lhs,"#kind");
    const int klhs = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    lua_getfield(ctx->L,rhs,"#kind");
    const int krhs = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);
    if( klhs != BS_ListType && krhs != BS_ListType )
        return 0;
    if( klhs == BS_ListType && krhs == BS_ListType && sameType(ctx,lhs,rhs) )
        return 1; // both lists
    if( klhs == BS_ListType )
    {
        lua_getfield(ctx->L,lhs,"#type");
        const int res = sameType(ctx,-1,rhs) || isSameOrSubclass(ctx, -1, rhs) || isInEnum(ctx,-1,rhs);
        lua_pop(ctx->L,1);
        return res ? 2 : 0; // lhs is list, rhs is element
    }
    if( krhs == BS_ListType )
    {
        lua_getfield(ctx->L,rhs,"#type");
        const int res = sameType(ctx,lhs,-1) || isSameOrSubclass(ctx, lhs, -1) || isInEnum(ctx,lhs, -1);
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
        error2(ctx, qmark->loc.row, qmark->loc.col,"expecting a boolean expression left of '?'" );
    lua_pop(ctx->L,1);

    if( ctx->skipMode )
    {
        lua_pop(ctx->L,2); // value, type
        expression(ctx,scope,lhsType);
        BSToken t = nextToken(ctx);
        if( t.tok != Tok_Colon )
            error2(ctx, t.loc.row, t.loc.col,"expecting ':'" );
        expression(ctx,scope,lhsType);
        // stack: value, type, value, type
        if( !sameType(ctx, -1,-3) )
            error2(ctx, t.loc.row, t.loc.col,"expression left and right of ':' must be of same type" );
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
                error2(ctx, t.loc.row, t.loc.col,"expecting ':'" );
            ctx->skipMode = 1;
            expression(ctx,scope,lhsType);
            ctx->skipMode = 0;
            if( !sameType(ctx, -1,-3) )
                error2(ctx, t.loc.row, t.loc.col,"expression left and right of ':' must be of same type" );
            lua_pop(ctx->L,2); // value, type
        }else
        {
            ctx->skipMode = 1;
            expression(ctx,scope,lhsType);
            ctx->skipMode = 0;
            BSToken t = nextToken(ctx);
            if( t.tok != Tok_Colon )
                error2(ctx, t.loc.row, t.loc.col,"expecting ':'" );
            expression(ctx,scope,lhsType);
            if( !sameType(ctx, -1,-3) )
                error2(ctx, t.loc.row, t.loc.col,"expression left and right of ':' must be of same type" );
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
                error2(ctx, lbrack->loc.row, lbrack->loc.col,"incompatible type" );
            lua_pop(ctx->L,1);
            lua_pushvalue(ctx->L,lhsType);
        }else
            error2(ctx, lbrack->loc.row, lbrack->loc.col,"cannot determine list type" );
    }else
    {
        int n = 0;
        lua_createtable(ctx->L,0,0);
        const int list = lua_gettop(ctx->L);
        expression(ctx,scope,0);

        lua_getfield(ctx->L,-1,"#kind");
        const int kr = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);

        if( lhsType && kr == BS_ClassDecl )
        {
            lua_getfield(ctx->L,lhsType,"#kind");
            int kl = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            if( kl == BS_ListType )
            {
                lua_getfield(ctx->L,lhsType,"#type");
                if( !isSameOrSubclass(ctx,-1,-2) )
                    error2(ctx, t.loc.row, t.loc.col,"the element is not compatible with the class" );
                // replace the type of the first expression by lhsType
                lua_replace(ctx->L,-2);
            }// else: the assignment type check produces the error
        }

        // store the first element in the list
        lua_pushvalue(ctx->L,-2);
        lua_rawseti(ctx->L,list,++n);
        lua_remove(ctx->L,-2);
        // take the type of the first element as reference
        const int type = lua_gettop(ctx->L);

        t = peekToken(ctx,1);
        if( t.tok == Tok_Comma )
        {
            nextToken(ctx); // ignore comma
            t = peekToken(ctx,1);
        }
        while( t.tok != Tok_Rbrack && t.tok != Tok_Eof )
        {
            expression(ctx,scope,0);
            if( !sameType(ctx,type,-1) && !isSameOrSubclass(ctx,type,-1) )
                error2(ctx, t.loc.row, t.loc.col,"all elements of the list literal must have compatible types" );
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
            error2(ctx, lbrack->loc.row, lbrack->loc.col,"non terminated array literal" );
        else
            nextToken(ctx); // eat rbrack
        // stack: list, element type
        lua_createtable(ctx->L,0,0);
        lua_pushinteger(ctx->L,BS_ListType);
        lua_setfield(ctx->L,-2, "#kind" );
        lua_pushvalue(ctx->L,type);
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
                    error2(ctx, t.loc.row, t.loc.col,"cannot call this procedure like a function" );
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
            error2(ctx, t.loc.row, t.loc.col,"expecting ')' here" );
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
                error2(ctx, t.loc.row, t.loc.col,"unary operator only applicable to integer or real types" );
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
                error2(ctx, t.loc.row, t.loc.col,"unary operator only applicable to boolean types" );
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
        unexpectedToken(ctx,&t);
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
    const int l = isListAndElemType(ctx,-3,-1);
    if( l )
    {
        const int nl = l == 1 || l == 2 ? lua_objlen(ctx->L,lhs) : 0;
        const int nr = l == 1 || l == 3 ? lua_objlen(ctx->L,rhs) : 0;
        if( tok->tok == Tok_Star )
        {
            if( l != 1 && l != 2 )
                error2(ctx, tok->loc.row, tok->loc.col,"only list * list or list * element supported" );
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
            error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to list operand type" );
    }else if( !sameType(ctx,lhs+1, rhs+1) )
        error2(ctx, tok->loc.row, tok->loc.col,"operator requires the same type on both sides" );
    else
    {
        lua_getfield(ctx->L,-1,"#kind");
        const int k = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        if( k != BS_BaseType )
            error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
        lua_getfield(ctx->L,-1,"#type");
        const int t = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        switch( t )
        {
        case BS_boolean:
            if( tok->tok == Tok_2Amp )
                lua_pushboolean(ctx->L, lua_toboolean(ctx->L,lhs) && lua_toboolean(ctx->L,rhs) );
            else
                error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to boolean operands" );
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
                error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to number operands" );
                break;
            }
            lua_replace(ctx->L,lhs);
            lua_pop(ctx->L,2);
            break;
        default:
            error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
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
        error2(ctx, tok->loc.row, tok->loc.col,"right side cannot be an absolute path");
        break;
    case 2:
        error2(ctx, tok->loc.row, tok->loc.col,"right side cannot be appended to given left side" );
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
                return 2; // right side cannot be appended to given left side
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
    const int l = isListAndElemType(ctx,-3,-1);
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
                error2(ctx, tok->loc.row, tok->loc.col,"only list minus list or list minus element supported" );
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
            error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to list operand type" );
    }else if( !sameType(ctx,lhs+1, rhs+1) )
    {
        error2(ctx, tok->loc.row, tok->loc.col,"operator requires the same type on both sides" );
    }else
    {
        lua_getfield(ctx->L,lhs+1,"#kind");
        const int k = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);
        if( k != BS_BaseType )
            error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
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
                error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to boolean operands" );
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
                error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to number operands" );
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
                error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to string operands" );
            break;
        case BS_path:
            if( tok->tok == Tok_Plus )
            {
                addPath(ctx,tok,lhs,rhs);
                lua_replace(ctx->L,lhs);
                lua_pop(ctx->L,2);
            }else
                error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to path operands" );
            break;
        default:
            error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
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
            error2(ctx, tok->loc.row, tok->loc.col,"comparison operator only applicable to ASCII strings");
        str++;
    }
}

static void evalRelation(BSParserContext* ctx, BSToken* tok)
{
    // in: // value, type, value, type
    BS_BEGIN_LUA_FUNC(ctx,-2); // value, type
    const int l = isListAndElemType(ctx,-3,-1);
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
            error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to operand types" );
    }else if( !sameType(ctx,lhs+1, rhs+1) && !isInEnum(ctx,lhs+1,rhs) && !isInEnum(ctx,rhs+1,lhs) )
    {
        error2(ctx, tok->loc.row, tok->loc.col,"operator requires the same base type on both sides" );
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
            error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
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
                    error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to operand type" );
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
                    error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to number type" );
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
                    error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to string type" );
                    break;
                }
                break;
            case BS_path:
                if( tok->tok == Tok_2Eq || tok->tok == Tok_BangEq )
                {
                    const int eq = lua_equal(ctx->L,lhs,rhs);
                    lua_pushboolean(ctx->L, tok->tok == Tok_2Eq ? eq : !eq );
                }else
                    error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to path type" );
                break;
            default:
                error2(ctx, tok->loc.row, tok->loc.col,"operator is not applicable to given operand type" );
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
    BS_BEGIN_LUA_FUNC(ctx,0);
    lua_createtable(ctx->L,0,0); // create a temporary block definition for the local declarations
    const int blockdecl = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L,BS_BlockDef);
    lua_setfield(ctx->L,blockdecl,"#kind");
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

    lua_pop(ctx->L,2); // blockdecl, blockinst
    BS_END_LUA_FUNC(ctx);
}

static void parampath(BSParserContext* ctx, BSIdentDef* id)
{
    BS_BEGIN_LUA_FUNC(ctx,2);
    lua_pushlstring(ctx->L,id->name, id->len);
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
        error2(ctx, t.loc.row, t.loc.col,"expecting 'var', 'let' or 'param'");
    BSIdentDef id = identdef(ctx, scope);

    if( kind == Tok_param && id.visi != BS_Private )
        error2(ctx, id.loc.row, id.loc.col,"visibility cannot be set for parameters (assumed to be public)");
    if( kind == Tok_param && scope != &ctx->module )
        error2(ctx, t.loc.row, t.loc.col,"parameters are only supported on module level");

    lua_createtable(ctx->L,0,0);
    const int var = lua_gettop(ctx->L);
    lua_pushinteger(ctx->L,BS_VarDecl);
    lua_setfield(ctx->L,var,"#kind");
    lua_pushboolean(ctx->L,kind == Tok_let || kind == Tok_param);
    lua_setfield(ctx->L,var,"#ro");

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
            error2(ctx, t.loc.row, t.loc.col,"class instance variables require an explicit type" );
        lua_getfield(ctx->L,explicitType,"#kind");
        if( lua_tointeger(ctx->L,-1) != BS_ClassDecl )
            error2(ctx, t.loc.row, t.loc.col,"constructors are only supported for class instances" );
        lua_pop(ctx->L,1);
        if( scope != &ctx->module )
            error2(ctx, t.loc.row, t.loc.col,"class instance variables only supported on module level");
        if( kind == Tok_param )
            error2(ctx, t.loc.row, t.loc.col,"parameter can only be of basic type" );

        lua_createtable(ctx->L,0,0);
        const int classInst = lua_gettop(ctx->L);
        // set the class of the instance
        lua_pushvalue(ctx->L,explicitType);
        lua_setmetatable(ctx->L, classInst);
        lua_pushvalue(ctx->L,var);
        lua_setfield(ctx->L,classInst,"#decl");

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

        nestedblock(ctx,scope,classInst, &t, pascal);

        if( pascal )
        {
            t = nextToken(ctx);
            if( t.tok != Tok_end )
                error2(ctx, id.loc.row, id.loc.col,"expecting 'end'" );
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
                error2(ctx, t.loc.row, t.loc.col,"type of the right hand expression is not compatible" );
        }else
        {
            // use the expression type as the var type
            if( lua_isnil(ctx->L,type) )
                error2(ctx, t.loc.row, t.loc.col,"type of the right hand expression cannot be infered" );
            lua_pushvalue(ctx->L,type);
            lua_setfield(ctx->L,var,"#type");
        }

        lua_getfield(ctx->L,type,"#kind");
        const int klt = lua_tointeger(ctx->L,-1);
        lua_pop(ctx->L,1);

        if( klt == BS_ClassDecl || klt == BS_ListType )
        {
            if( kind == Tok_param )
                error2(ctx, t.loc.row, t.loc.col,"parameter can only be of basic type" );

            if( kind == Tok_var && ro > 0 )
                error2(ctx, t.loc.row, t.loc.col,"cannot assign immutable object to var" );
        }
        if( klt != BS_ClassDecl && id.visi == BS_PublicDefault )
            error2(ctx, id.loc.row, id.loc.col,"'!' is not applicable here" );

        // store the value to the var instance
        lua_getfield(ctx->L, scope->table, "#inst" );
        const int inst = lua_gettop(ctx->L);
        lua_pushlstring(ctx->L,id.name, id.len);
        lua_pushvalue(ctx->L,type-1);
        lua_rawset(ctx->L,inst);

        if( kind == Tok_param )
        {
            parampath(ctx,&id);

            const int accessible = lua_tointeger(ctx->L,-1);
            lua_pop(ctx->L,1);
            const int desig = lua_gettop(ctx->L);
            lua_pushvalue(ctx->L,desig);
            lua_rawget(ctx->L,BS_Params);
            // stack: desig, val or nil
            if( !lua_isnil(ctx->L,-1) )
            {
                if( !accessible )
                    error2(ctx, id.loc.row, id.loc.col,
                           "the parameter %s cannot be set because it is not visible from the root directory",
                           lua_tostring(ctx->L,desig));
                // remove the used param from the table
                lua_pushvalue(ctx->L,desig);
                lua_pushnil(ctx->L);
                lua_rawset(ctx->L,BS_Params);
                // stack: desig, val
                const char* val = lua_tostring(ctx->L,desig+1);
                uchar len;
                const uint ch = unicode_decode_utf8((const uchar*)val, &len );
                if( len == 0 )
                    error2(ctx, id.loc.row, id.loc.col,"passing invalid value to parameter %s: %s",
                           lua_tostring(ctx->L,desig), lua_tostring(ctx->L,desig+1));
                if( unicode_isdigit(ch) || ch == '`' || ch == '$' || ch == '/' || ch == '.'
                        || ch == '\'' || ch == '"' )
                {
                    lua_pushfstring(ctx->L, "parameter '%s': %s", lua_tostring(ctx->L,desig),
                                    lua_tostring(ctx->L,desig+1) );
                    BSLexer* l = bslex_openFromString(lua_tostring(ctx->L,desig+1),lua_tostring(ctx->L,-1));
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
                        error2(ctx, id.loc.row, id.loc.col,"unexpected parameter value type %s: %s",
                               lua_tostring(ctx->L,desig), lua_tostring(ctx->L,desig+1));
                        break;
                    }
                }else
                {
                    if( strcmp(lua_tostring(ctx->L,desig+1),"true") == 0 )
                    {
                        lua_getfield(ctx->L,ctx->builtins,"bool");
                        lua_pushboolean(ctx->L,1);
                    }else if( strcmp(lua_tostring(ctx->L,desig+1),"false") == 0 )
                    {
                        lua_getfield(ctx->L,ctx->builtins,"bool");
                        lua_pushboolean(ctx->L,0);
                    }else
                    {
                        lua_getfield(ctx->L,ctx->builtins,"string"); // assume string
                        lua_pushvalue(ctx->L,desig+1);
                    }
                }
                // stack: desig, valstr, valtype, val
                lua_getfield(ctx->L,var,"#type");
                // stack: desig, valstr, valtype, val, reftype
                if( !sameType(ctx,-1,-3) && !isInEnum(ctx,-1,-2) )
                    error2(ctx, t.loc.row, t.loc.col,"value passed in for parameter '%s' is incompatible",
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
        error2(ctx, t.loc.row, t.loc.col,"expecting '{' or '='" );

    lua_pop(ctx->L,2); // vardecl, explicitType
    BS_END_LUA_FUNC(ctx);
}

static void call(BSParserContext* ctx, BSScope* scope)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    resolveInstance(ctx,scope);
    lua_remove(ctx->L,-2);
    // stack: derefed declaration
    evalCall(ctx,scope);
    lua_pop(ctx->L,2); // not using return value
    BS_END_LUA_FUNC(ctx);
}

static void assignment(BSParserContext* ctx, BSScope* scope)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    BSToken t = peekToken(ctx,1);
    const int lro = resolveInstance(ctx,scope);
    // resolved container instance + derefed declaration
    const int lhs = lua_gettop(ctx->L) - 1;

    if( lro == 1 )
        error2(ctx, t.loc.row, t.loc.col,"cannot modify immutable object" );

    t = nextToken(ctx);
    switch(t.tok)
    {
    case Tok_Eq:
    case Tok_ColonEq:
    case Tok_PlusEq:
    case Tok_MinusEq:
    case Tok_StarEq:
        break;
    default:
        error2(ctx, t.loc.row, t.loc.col,"expecting '=', '+=', '-=' or '*='" );
        break;
    }

    lua_getfield(ctx->L,lhs+1,"#type");
    const int lt = lua_gettop(ctx->L);

    const int rro = expression(ctx,scope,lt);
    // value, type
    const int rhs = lua_gettop(ctx->L) - 1;

    const int l = isListAndElemType(ctx,lt,rhs+1);
    const int sub = isSameOrSubclass(ctx,lt,rhs+1);
    const int same = sameType(ctx,lt,rhs+1);
    const int inenum = isInEnum(ctx,lt,rhs);
    if( !same && !(l == 1 || l == 2) && !sub  && !inenum )
        error2(ctx, t.loc.row, t.loc.col,"left and right side are not assignment compatible" );
    if( l == 2 && t.tok == Tok_Eq )
        error2(ctx, t.loc.row, t.loc.col,"cannot assign an element to a list; use += instead" );

    lua_getfield(ctx->L,lt,"#kind");
    const int klt = lua_tointeger(ctx->L,-1);
    lua_pop(ctx->L,1);

    if( klt == BS_ClassDecl || klt == BS_ListType )
    {
        if( lro == 0 && rro != 0 && ( t.tok == Tok_Eq || t.tok == Tok_ColonEq ) )
            error2(ctx, t.loc.row, t.loc.col,"cannot assign immutable object to var" );
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
                error2(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
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
                error2(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
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
                error2(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
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
                error2(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
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
                error2(ctx, t.loc.row, t.loc.col,"operator is not applicable to given operand type" );
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
    BS_BEGIN_LUA_FUNC(ctx,0);
    BSToken t = nextToken(ctx); // eat if
    t = peekToken(ctx,1);
    expression(ctx,scope,0);
    lua_getfield(ctx->L,-1,"#type");
    if( lua_tointeger(ctx->L,-1) != BS_boolean )
        error2(ctx, t.loc.row, t.loc.col,"expecting a boolean if expression" );
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
                error2(ctx, t.loc.row, t.loc.col,"expecting a boolean if expression" );
            lua_pop(ctx->L,1); // type
            cond = lua_toboolean(ctx->L,-2);
            lua_pop(ctx->L,2); // value, type
            t = nextToken(ctx);
            if( t.tok != Tok_then )
                error2(ctx, t.loc.row, t.loc.col,"expecting 'then'" );
            if( !skipping )
                ctx->skipMode = !( cond && !done );
            nestedblock(ctx,scope,0,&t,1);
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
            if( !skipping )
                ctx->skipMode = 0;
            t = nextToken(ctx);
        }
        if( t.tok != Tok_end )
            error2(ctx, t.loc.row, t.loc.col,"expecting 'end'" );
    }else
    {
        if( t.tok != Tok_Lbrace )
            error2(ctx, t.loc.row, t.loc.col,"expecting '{'" );
        nestedblock(ctx,scope,0,&t,0);
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
                break;
            case Tok_Lbrace:
                nextToken(ctx); // eat lbrace
                nestedblock(ctx,scope,0,&t,0);
                break;
            default:
                error2(ctx, t.loc.row, t.loc.col,"expecting 'if' or '{'" );
                break;
            }
            if( !skipping )
                ctx->skipMode = 0;
        }
    }

    BS_END_LUA_FUNC(ctx);
}

static void block(BSParserContext* ctx, BSScope* scope, BSToken* inLbrace, int pascal)
{
    BS_BEGIN_LUA_FUNC(ctx,0);
    BSToken t = peekToken(ctx,1);
    while( !endOfBlock(&t,pascal) && t.tok != Tok_Eof )
    {
        if( t.tok == Tok_subdir && &ctx->module == scope )
            subdirectory(ctx);
        else
            switch( t.tok )
            {
            case Tok_subdir:
                subdirectory(ctx);
                break;
            case Tok_var:
            case Tok_let:
            case Tok_param:
                vardecl(ctx, scope);
                break;
            case Tok_type:
                typedecl(ctx, scope);
                break;
            case Tok_Hat:
            case Tok_Dot:
                // start of a designator
                assignment(ctx, scope);
                break;
            case Tok_if:
                condition(ctx, scope);
                break;
            case Tok_ident:
                {
                    BSToken t2 = peekToken(ctx,2);
                    switch( t2.tok )
                    {
                    case Tok_Eq:
                    case Tok_ColonEq:
                    case Tok_PlusEq:
                    case Tok_MinusEq:
                    case Tok_StarEq:
                    case Tok_Dot:
                        assignment(ctx, scope);
                        break;
                    case Tok_Lpar:
                        call(ctx, scope);
                        break;
                    default:
                        error2(ctx, t.loc.row, t.loc.col,"looks like an assignment or a call, but next token doesn't fit" );
                        break;
                    }
                }
                break;
            default:
                unexpectedToken(ctx, &t);
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
            error2(ctx, t.loc.row, t.loc.col,"unexpected '%s'", bslex_tostring(t.tok) );
        else if( !pascal )
            nextToken(ctx); // eat rbrace
    }else if( t.tok == Tok_Eof && inLbrace )
        error2(ctx, inLbrace->loc.row, inLbrace->loc.col,"non-terminated block" );
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

int bs_parse(lua_State* L)
{
    if( lua_isnil(L,BS_Params) )
    {
        lua_createtable(L,0,0);
        lua_replace(L,BS_Params);
    }
    if( lua_isnil(L,BS_NewModule) )
    {
        lua_createtable(L,0,0); // module definition
        const int module = lua_gettop(L);
        lua_pushinteger(L, BS_ModuleDef);
        lua_setfield(L,module,"#kind");
        lua_replace(L,BS_NewModule);
        lua_pushstring(L,".");
        lua_setfield(L,BS_NewModule,"#rdir"); // relative directory
    }

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

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
    if( strncmp(ctx.dirpath,"//",2) != 0 )
        luaL_error(L,"expecting absolute, normalized directory path: %s", ctx.dirpath );
    ctx.builtins = builtins;
    ctx.label = calcLabel(ctx.dirpath,calcLevel(L,BS_NewModule)+1);
    // BS_BEGIN_LUA_FUNC(&ctx,1); cannot use this here because module was created above already
    const int $stack = lua_gettop(L)+1;
    lua_pushvalue(L,BS_PathToSourceRoot);
    lua_setfield(L,BS_NewModule,"#dir");
    lua_pushvalue(L,BS_PathToSourceRoot);
    lua_pushstring(L,"/");
    lua_pushstring(L,"BUSY");
    lua_concat(L,3);
    ctx.filepath = lua_tostring(L,-1); // RISK
    lua_setfield(L,BS_NewModule,"#file");
    fprintf(stdout,"# analyzing %s\n",ctx.filepath);
    fflush(stdout);
    ctx.lex = bslex_open(bs_denormalize_path(ctx.filepath));
    if( ctx.lex == 0 )
    {
        lua_pop(L,1);
        lua_pushnil(L);
        lua_error(L);
    }

    block(&ctx,&ctx.module,0,0);
    lua_pushvalue(L,BS_NewModule);

    bslex_free(ctx.lex);
    BS_END_LUA_FUNC(&ctx);
    return 1;
}
