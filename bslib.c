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

#include "bslib.h"
#include "lauxlib.h"
#include "bslex.h"
#include "bsparser.h"
#include "bsrunner.h"
#include "bsqmakegen.h"
#include "bshost.h"
#include "bsunicode.h"
#include "bsvisitor.h"
#include <ctype.h>
#include <string.h>
#include <assert.h>

static int copy(lua_State* L)
{
    enum Params { FROMFILE = 1, TOFILE };

    BSPathStatus res = bs_normalize_path2(lua_tostring(L,FROMFILE));
    if( res != BS_OK )
        luaL_error(L,"invalid from-file: %s", lua_tostring(L,FROMFILE) );
    lua_pushstring(L, bs_global_buffer() );
    const int fromFile = lua_gettop(L);

    res = bs_normalize_path2(lua_tostring(L,TOFILE));
    if( res != BS_OK )
        luaL_error(L,"invalid to-file: %s", lua_tostring(L,TOFILE) );
    lua_pushstring(L, bs_global_buffer() );
    const int toFile = lua_gettop(L);

    bs_copy(lua_tostring(L,toFile),lua_tostring(L,fromFile));

    return 0;
}

static int bs_version(lua_State *L)
{
    lua_pushstring(L,BS_BSVERSION);
    return 1;
}

// returns: current directory in normalized form
static int bs_getcwd (lua_State *L)
{
    if( bs_cwd() == BS_OK )
        lua_pushstring(L, bs_global_buffer());
    else
        luaL_error(L,"getcwd: received non supported path from OS");
    return 1;
}

static int host_cpu(lua_State *L)
{
    BSCpu c = bs_host_cpu();
    lua_pushstring(L,c.name);
    lua_pushinteger(L,c.ver);
    return 2;
}

static int host_os(lua_State *L)
{
    lua_pushstring(L,bs_host_os());
    return 1;
}

static int host_wordsize(lua_State *L)
{
    lua_pushinteger(L,bs_wordsize());
    return 1;
}

static int host_compiler(lua_State *L)
{
    BSCompiler c = bs_host_compiler();
    lua_pushstring(L,c.name);
    lua_pushinteger(L,c.ver);
    return 2;
}


// returns: file path of the currently running application in normalized form
static int thisapp (lua_State *L)
{
    return bs_thisapp2(L);
}

static void push_normalized(lua_State *L, int path)
{
    int res = bs_normalize_path2(lua_tostring(L,path));
    switch(res)
    {
    case BS_OK:
        break;
    case BS_NotSupported:
        luaL_error(L,"path format is not supported: %s", lua_tostring(L,path) );
        break;
    case BS_InvalidFormat:
        luaL_error(L,"path format is invalid: %s", lua_tostring(L,path) );
        break;
    case BS_OutOfSpace:
        luaL_error(L,"path is too long to be handled: %s", lua_tostring(L,path) );
        break;
    default:
        luaL_error(L,"unknown error for path: %s", lua_tostring(L,path) );
        break;
    }

    lua_pushstring(L, bs_global_buffer());
    if( strncmp(bs_global_buffer(),"//",2) != 0 )
    {
        // relative path
        res = bs_cwd();
        if( res != BS_OK )
            luaL_error(L,"getcwd delivered a path not supported by this application" );
        lua_pushstring(L,bs_global_buffer());
        res = bs_add_path(L,-1,-2);
        if( res != 0 )
            luaL_error(L,"creating absolute path from provided root gives an error: %s", lua_tostring(L,1) );
        // stack: rhs, lhs, abspath
        lua_replace(L,-3);
        lua_pop(L,1);
        // stack: abspath
    }
}

// NOTE: if BUSY is built information about the OS and toolchain is melted into the executable and doesn't have
//   to be explicitly set when BUSY is used.
int bs_compile (lua_State *L)
{
    enum { SOURCE_DIR = 1, BUILD_DIR, PARAMS };
    int i;
    for( i = lua_gettop(L); i < 3; i++ )
        lua_pushnil(L); // add the missing args
    const int top = lua_gettop(L);

    if( lua_isnil(L,SOURCE_DIR) )
    {
        lua_pushstring(L, "..");
        lua_replace(L,SOURCE_DIR);
    }
    if( lua_isnil(L,BUILD_DIR) )
    {
        lua_pushstring(L, "./output");
        lua_replace(L,BUILD_DIR);
    }
    if( lua_isnil(L,PARAMS) )
    {
        lua_createtable(L,0,0);
        lua_replace(L,PARAMS);
    }

    // set lua path so no environment can intervene
    lua_getglobal(L, "package");
    lua_pushstring(L,"./?.lua");
    lua_setfield(L,-2,"path");
    lua_pushstring(L,"");
    lua_setfield(L,-2,"cpath");
    lua_pop(L,1);

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);
    lua_getfield(L,-1,"#inst");
    const int binst = lua_gettop(L);

    push_normalized(L,BUILD_DIR);
    lua_setfield(L,binst,"root_build_dir");
    push_normalized(L,SOURCE_DIR);
    fprintf(stdout,"# running parser\n# root source directory is %s\n",lua_tostring(L,-1));
    fflush(stdout);
    lua_setfield(L,binst,"root_source_dir");

    lua_pushnil(L);  /* first key */
    while (lua_next(L, PARAMS) != 0) {
        // go through all params and see if there is a global param with the same name and if so apply it
        const int key = lua_gettop(L)-1;

        lua_pushvalue(L,key);
        lua_rawget(L,builtins);
        if( !lua_isnil(L,-1) )
        {
            const int decl = lua_gettop(L);
            lua_getfield(L,decl,"#kind");
            const int k = lua_tointeger(L,-1);
            lua_getfield(L,decl,"#rw");
            const int rw = lua_tointeger(L,-1);
            lua_pop(L, 2); // k, rw
            lua_getfield(L,decl,"#type");
            const int refType = lua_gettop(L);
            if( k == BS_VarDecl && rw == BS_param )
            {
                lua_pushvalue(L,key);
                if( bs_getAndCheckParam( L, builtins, PARAMS, key, 1, refType ) == 0 )
                {
                    if( !lua_isnil(L,-1) )
                        // the param value is ok, assign it
                        lua_rawset(L,binst); // eats value
                    else
                        lua_pop(L,1); // value
                }else
                    lua_error(L);
            }
            lua_pop(L, 1); // refType
        }
        lua_pop(L, 2); // value, decl
    }

    lua_pop(L,2); // builtins, binst

    lua_createtable(L,0,0);
    lua_setglobal(L,"#xref"); // overwrite an existing #xref if present

    lua_createtable(L,0,0);
    lua_createtable(L,0,0);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setglobal(L,"#refs"); // overwrite an existing #refs if present

    lua_pushcfunction(L, bs_parse);

    push_normalized(L,SOURCE_DIR);

    lua_createtable(L,0,0); // module definition
    const int module = lua_gettop(L);
    lua_pushinteger(L, BS_ModuleDef);
    lua_setfield(L,module,"#kind");
    lua_pushstring(L,"."); // start rdir from '.'
    lua_setfield(L,module,"#rdir"); // virtual directory relative to source root
    lua_pushstring(L,"."); // start rdir from '.'
    lua_setfield(L,module,"#fsrdir"); // file system directory relative to source root

    lua_pushvalue(L,module);
    lua_setglobal(L, "#root");

    lua_pushvalue(L,PARAMS);

    lua_call(L,3,1);
    // module is on the stack

    lua_pushnil(L);
    while (lua_next(L, 3) != 0)
    {
        luaL_error(L,"cannot set unknown parameter: %s", lua_tostring(L,-2) );
        lua_pop(L, 1);
    }

    const int bottom = lua_gettop(L);
    assert( top + 1 == bottom );
    return 1;
}

static int resolvedesig(lua_State *L, const char* desig, int root)
{
    const char* p = desig;
    const char* q = desig;
    lua_pushvalue(L,root);
    const int module = lua_gettop(L);
    while( 1 )
    {
        uchar len = 0;
        const uint ch = unicode_decode_utf8((const uchar*)q,&len);
        if( len == 0 )
            luaL_error(L,"the passed-in product designator has invalid UTF-8 format: %s",desig);
        if( ch == 0 || ch == '.' )
        {
            if( p == q )
                luaL_error(L,"the passed-in product designator has invalid syntax: %s",desig);
            if( p != desig )
            {
                if( !lua_istable(L,-1) )
                {
                    lua_pushlstring(L,desig,p-desig-1);
                    luaL_error(L,"'%s' of passed-in designator '%s' cannot be dereferenced",lua_tostring(L,-1),desig);
                }
                assert(lua_istable(L,-1));
                lua_getfield(L,-1,"#kind");
                const int k = lua_tointeger(L,-1);
                lua_pop(L,1);
                if( k != BS_ModuleDef )
                {
                    lua_pushlstring(L,desig,p-desig-1);
                    luaL_error(L,"'%s' of passed-in designator '%s' must be a subdir declaration",lua_tostring(L,-1),desig);
                }
                lua_getfield(L,-1,"#visi");
                const int v = lua_tointeger(L,-1);
                lua_pop(L,1);
                if( v != BS_Public )
                {
                    lua_pushlstring(L,desig,p-desig-1);
                    luaL_error(L,"subdir '%s' of passed-in designator '%s' is not public",lua_tostring(L,-1),desig);
                }
            }
            lua_pushlstring(L,p,q-p);
            lua_rawget(L,module);
            if( lua_isnil(L,-1) )
            {
                lua_pushlstring(L,p,q-p);
                luaL_error(L,"identifier '%s' of passed-in designator '%s' not found",lua_tostring(L,-1),desig);
            }
            p = q + 1;
            if( ch == '.' && *p == 0 )
                luaL_error(L,"the passed-in product designator has invalid syntax: %s",desig);
            lua_replace(L,module);
        }
        if( ch == 0 )
            break;
        q += len;
    }
    return 1; // returns found decl or doesn't return
}

static int isproductvardecl(lua_State *L, int t, int bi)
{
    if( t <= 0 )
        t += lua_gettop(L) + 1;
    if( !lua_istable(L,t) )
        return 0;
    lua_getfield(L,t,"#kind");
    const int k = lua_tointeger(L,-1);
    lua_pop(L,1);
    if( k != BS_VarDecl )
        return 0;
    lua_getfield(L,t,"#type");
    lua_getfield(L,bi,"Product");
    const int res = bs_isa(L,-1,-2);
    lua_pop(L,2);
    return res;
}

static int fetchInstOfDecl(lua_State* L, int decl)
{
    const int top = lua_gettop(L);
    if( decl < 0 )
        decl += top + 1;
    lua_getfield(L,decl,"#owner");
    lua_getfield(L,-1,"#inst");
    lua_replace(L,-2);
    // stack: modinst
    lua_getfield(L,decl,"#name");
    lua_rawget(L,-2);
    // stack: modinst, classinst
    lua_replace(L,-2);
    assert(top + 1 == lua_gettop(L) );
    return 1;
}

int bs_findProductsToProcess(lua_State *L )
{
    enum { ROOT = 1, PRODS, builtins };
    lua_createtable(L,10,0);
    const int res = lua_gettop(L);
    int n = 0;
    if( lua_isnil(L,PRODS) )
    {
        // search for default products an run them
        const int len = lua_objlen(L,ROOT);
        int i;
        for( i = 1; i <= len; i++ )
        {
            lua_rawgeti(L,ROOT,i);
            const int decl = lua_gettop(L);
            if( isproductvardecl(L,decl,builtins) )
            {
                lua_getfield(L,-1,"#visi");
                const int visi = lua_tointeger(L,-1);
                lua_pop(L,1);
                if( visi == BS_PublicDefault )
                {
                    fetchInstOfDecl(L,decl);
                    lua_rawseti(L,res,++n);
                }
            }
            lua_pop(L,1); // decl
        }
        if( lua_objlen(L,res) == 0 )
            luaL_error(L,"the module doesn't have any default product declarations");
    }else
    {
        // run through all products in the set; get insts; error if not visible
        lua_pushnil(L);
        while (lua_next(L, PRODS) != 0)
        {
            resolvedesig(L,lua_tostring(L,-2),ROOT);
            const int decl = lua_gettop(L);
            if( isproductvardecl(L,decl,builtins) )
            {
                lua_getfield(L,-1,"#visi");
                const int visi = lua_tointeger(L,-1);
                lua_pop(L,1);
                if( visi >= BS_Public )
                {
                    fetchInstOfDecl(L,decl);
                    lua_rawseti(L,res,++n);
                }else
                    luaL_error(L,"the declaration is not visible from outside: %s", lua_tostring(L,-3));
            }else
                luaL_error(L,"no valid product declaration: %s", lua_tostring(L,-3));
            lua_pop(L,2); // key, value
        }
    }
    return 1; // leaves res on stack
}

int bs_execute (lua_State *L)
{
    enum { ROOT = 1, PRODS };
    const int top = lua_gettop(L);

    if( !lua_istable(L,ROOT) )
        luaL_error(L,"expecting a module definition");
    lua_getfield(L,ROOT,"#kind");
    if( lua_tointeger(L,-1) != BS_ModuleDef )
        luaL_error(L,"expecting a module definition");
    else
        lua_pop(L,1); // kind

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getfield(L,ROOT,"#dir");
    const int source_dir = lua_gettop(L);
    fprintf(stdout,"# running build for %s\n",lua_tostring(L,source_dir));
    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);
    lua_getfield(L,binst,"root_build_dir");
    const int build_dir = lua_gettop(L);
    fprintf(stdout,"# root build directory is %s\n",lua_tostring(L,-1));
    fflush(stdout);

    // we create build dir tree here because target dependencies can skip directories and cause
    // mkdir errors if levels are (temporarily) missing.
    lua_pushcfunction(L, bs_createBuildDirs);
    lua_pushvalue(L,ROOT);
    lua_pushvalue(L,build_dir);
    lua_call(L,2,0);

    lua_pop(L,3); // source_dir, binst, build_dir

    lua_pushcfunction(L, bs_findProductsToProcess);
    lua_pushvalue(L,ROOT);
    lua_pushvalue(L,PRODS);
    lua_pushvalue(L,builtins);
    lua_call(L,3,1);
    lua_replace(L,PRODS);

    lua_pop(L,1); // builtins

    // build all products in the set; first check for error message dependents
    size_t i;
    for( i = 1; i <= lua_objlen(L,PRODS); i++ )
    {
        lua_pushcfunction(L, bs_precheck);
        lua_rawgeti(L,PRODS,i);
        lua_call(L,1,0);
    }
    for( i = 1; i <= lua_objlen(L,PRODS); i++ )
    {
        lua_pushcfunction(L, bs_run);
        lua_rawgeti(L,PRODS,i);
        lua_call(L,1,0);
    }

    const int bottom = lua_gettop(L);
    assert( top == bottom );
    return 0;
}

static int Test_BSBeginOp(BSBuildOperation op, const char* command, int t, int o, void* data)
{
    switch(op)
    {
    case BS_Compile:
        fprintf(stdout,"COMPILE: ");
        break;
    case BS_LinkExe:
    case BS_LinkDll:
    case BS_LinkLib:
        fprintf(stdout,"LINK: ");
        break;
    case BS_RunMoc:
        fprintf(stdout,"MOC: ");
        break;
    case BS_RunRcc:
        fprintf(stdout,"RCC: ");
        break;
    case BS_RunUic:
        fprintf(stdout,"UIC: ");
        break;
    case BS_RunLua:
        fprintf(stdout,"LUA: ");
        break;
    case BS_Copy:
        fprintf(stdout,"COPY: ");
        break;
    default:
        fprintf(stdout,"BEGIN OP: %d ", op);
        break;
    }
    fprintf(stdout, "%s\n", command );
    fflush(stdout);

    return 0;
}

static void Test_BSOpParam(BSBuildParam p, const char* value, void* data)
{
    switch(p)
    {
    case BS_infile:
        fprintf(stdout,"  INFILE: ");
        break;
    case BS_outfile:
        fprintf(stdout,"  OUTFILE: ");
        break;
    case BS_cflag:
        fprintf(stdout,"  CFLAG: ");
        break;
    case BS_define:
        fprintf(stdout,"  DEFINE: ");
        break;
    case BS_include_dir:
        fprintf(stdout,"  INCLUDEDIR: ");
        break;
    case BS_ldflag:
        fprintf(stdout,"  LDFLAG: ");
        break;
    case BS_lib_dir:
        fprintf(stdout,"  LIBDIR: ");
        break;
    case BS_lib_name:
        fprintf(stdout,"  LIBNAME: ");
        break;
    default:
        fprintf(stdout,"  PARAM: ");
        break;
    }
    fprintf(stdout, "%s\n", value );
    fflush(stdout);
}

static void Test_BSForkGroup(int n, void* data)
{
    if( n >= 0 )
        fprintf(stdout,"BEGIN PARALLEL: %d\n", n);
    else
        fprintf(stdout,"END PARALLEL\n");
    fflush(stdout);
}

// param: what, root module def
// opt param: set of product desigs to be built
static int bs_generate (lua_State *L)
{
    enum { WHAT = 1, ROOT, PRODS };
    const int top = lua_gettop(L);

    if( !lua_isstring(L,WHAT) )
        luaL_error(L,"invalid generator selected");

    if( !lua_istable(L,ROOT) )
        luaL_error(L,"expecting a module definition");
    lua_getfield(L,ROOT,"#kind");
    if( lua_tointeger(L,-1) != BS_ModuleDef )
        luaL_error(L,"expecting a module definition");
    else
        lua_pop(L,1); // kind

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getfield(L,ROOT,"#dir");
    const int source_dir = lua_gettop(L);
    fprintf(stdout,"# running generator '%s' for %s\n",lua_tostring(L,WHAT),lua_tostring(L,source_dir));
    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);
    lua_getfield(L,binst,"root_build_dir");
    const int build_dir = lua_gettop(L);
    fprintf(stdout,"# root output directory is %s\n",lua_tostring(L,-1));
    fflush(stdout);
    lua_pop(L,3); // source_dir, binst, build_dir

    lua_pushcfunction(L, bs_findProductsToProcess);
    lua_pushvalue(L,ROOT);
    lua_pushvalue(L,PRODS);
    lua_pushvalue(L,builtins);
    lua_call(L,3,1);
    lua_replace(L,PRODS);

    lua_pop(L,1); // builtins

    // generate all products in the set; first check for error message dependents
    size_t i;
    for( i = 1; i <= lua_objlen(L,PRODS); i++ )
    {
        lua_pushcfunction(L, bs_precheck);
        lua_rawgeti(L,PRODS,i);
        lua_call(L,1,0);
    }

    if( strcmp(lua_tostring(L,WHAT),"qmake") == 0 )
    {
        lua_pushcfunction(L, bs_genQmake);
        lua_pushvalue(L,ROOT);
        lua_pushvalue(L,PRODS);
        lua_call(L,2,0);
    }else if( strcmp(lua_tostring(L,WHAT),"test") == 0 )
    {
        for( i = 1; i <= lua_objlen(L,PRODS); i++ )
        {
            lua_pushcfunction(L, bs_visit);
            lua_rawgeti(L,PRODS,i);
            BSVisitorCtx* ctx = (BSVisitorCtx*)lua_newuserdata(L, sizeof(BSVisitorCtx) );
            ctx->d_data = 0;
            ctx->d_log = 0;
            ctx->d_end = 0;
            ctx->d_begin = Test_BSBeginOp;
            ctx->d_param = Test_BSOpParam;
            ctx->d_fork = Test_BSForkGroup;
            lua_call(L,2,0);
        }
    }else
        luaL_error(L,"unknown generator '%s'", lua_tostring(L,WHAT));

    const int bottom = lua_gettop(L);
    assert( top == bottom );
    return 0;
}

static const luaL_Reg bslib[] = {
    {"compile",      bs_compile},
    {"execute",      bs_execute},
    {"generate",      bs_generate},
    {"dump",      bs_dump},
    {"getcwd",      bs_getcwd},
    {"thisapp",      thisapp},
    {"moc", bs_runmoc},
    {"moc_name", bs_mocname},
    {"copy", copy},
    {"run", bs_run},
    {"cpu", host_cpu },
    {"os", host_os },
    {"wordsize", host_wordsize },
    {"compiler", host_compiler },
    {"version", bs_version },
    {NULL, NULL}
};

LUALIB_API int bs_open_busy (lua_State *L) {
  luaL_register(L, BS_BSLIBNAME, bslib);
  return 1;
}
