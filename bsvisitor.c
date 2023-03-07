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

#include "bsvisitor.h"
#include "bshost.h"
#include "bsparser.h" 
#include "bslex.h" // default_logger
#include "lauxlib.h"
#include <assert.h>
#include <string.h>

enum VISIT_ARGS { PRODINST = 1, CTX };

static int calcdesig(lua_State* L, int decl)
{
    return bs_declpath(L,decl,".");
}

static int isa(lua_State* L, int builtins, int cls, const char* what )
{
    lua_getfield(L,builtins,what);
    const int res = bs_isa(L,-1,cls);
    lua_pop(L,1);
    return res;
}

static void addflags(lua_State* L, int list, int out)
{
    if( list < 0 )
        list += lua_gettop(L) + 1;

    const int n = lua_objlen(L,list);
    int i;
    for( i = 1; i <= n; i++ )
    {
        lua_pushvalue(L,out);
        lua_pushstring(L," ");
        lua_rawgeti(L,list,i);
        lua_concat(L,3);
        lua_replace(L,out);
    }
}

static void addPath(lua_State* L, int lhs, int rhs)
{
    if( *lua_tostring(L,rhs) == '/' )
        lua_pushvalue(L, rhs);
    else if( bs_add_path(L,lhs,rhs) )
        luaL_error(L,"creating absolute path from provided root gives an error: %s %s",
                   lua_tostring(L,lhs), lua_tostring(L,rhs) );
}

static void copyItems(lua_State* L, int inlist, int outlist, BSOutKind what )
{
    lua_getfield(L,inlist,"#kind");
    const unsigned k = lua_tointeger(L,-1);
    lua_pop(L,1); // kind

    if( k == BS_Mixed )
    {
        size_t i, len = lua_objlen(L,inlist);
        for( i = 1; i <= len; i++ )
        {
            lua_rawgeti(L,inlist,i);
            const int sublist = lua_gettop(L);
            copyItems(L,sublist,outlist,what);
            lua_pop(L,1); // sublist
        }
    }else if( k == what )
    {
        // also works with BS_DynamicLib, BS_StaticLib and BS_Executable
        size_t i, len = lua_objlen(L,inlist);
        for( i = 1; i <= len; i++ )
        {
            lua_rawgeti(L,inlist,i);
            lua_rawseti(L,outlist, lua_objlen(L,outlist)+1 );
        }
    }
}

static void prefixCmd(lua_State* L, int cmd, int binst, int to_host)
{
    const char* toolchain_prefix = "#toolchain_prefix";
    const char* toolchain_path = "#toolchain_path";
    if( !to_host )
    {
        toolchain_prefix = "target_toolchain_prefix";
        toolchain_path = "target_toolchain_path";
    }

    lua_getfield(L,binst,toolchain_prefix);
    if( !lua_isnil(L,-1) && *lua_tostring(L,-1) != 0 )
    {
        lua_pushvalue(L,cmd);
        lua_concat(L,2);
        lua_replace(L,cmd); // prefix cmd with toolchain_prefix
    }else
        lua_pop(L,1);

    lua_getfield(L,binst,toolchain_path);
    if( !lua_isnil(L,-1) && strcmp( lua_tostring(L,-1), "." ) != 0 )
    {
        lua_pushfstring(L,"%s/%s", bs_denormalize_path(lua_tostring(L,-1)), lua_tostring(L,cmd) );
        lua_replace(L,cmd); // prefix cmd with path
        lua_pop(L,1);
    }else
        lua_pop(L,1);
}

static void emitFlags(lua_State* L,int inst, BSVisitorCtx* ctx, BSBuildParam paramType, const char* field)
{
    assert( ctx->d_param != 0 );

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        // TODO: check for circular deps
        emitFlags(L, config, ctx, paramType, field);
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    lua_getfield(L,inst, field);
    const int list = lua_gettop(L);
    const size_t n = lua_objlen(L,list);
    for( i = 1; i <= n; i++ )
    {
        lua_rawgeti(L,list,i);
        ctx->d_param(paramType,lua_tostring(L,-1),ctx->d_data);
        lua_pop(L,1);
    }
    lua_pop(L,1);
}

static void emitPaths(lua_State* L,int inst, BSVisitorCtx* ctx, BSBuildParam paramType, const char* field)
{
    assert( ctx->d_param != 0 );

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        // TODO: check for circular deps
        emitPaths(L,config, ctx, paramType, field);
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,inst,field);
    const int incls = lua_gettop(L);

    for( i = 1; i <= lua_objlen(L,incls); i++ )
    {
        lua_rawgeti(L,incls,i);
        const int path = lua_gettop(L);
        if( *lua_tostring(L,-1) != '/' )
        {
            // relative path
            addPath(L,absDir,path);
            lua_replace(L,path);
        }
        ctx->d_param(paramType, bs_denormalize_path(lua_tostring(L,path)),ctx->d_data);
        lua_pop(L,1); // path
    }
    lua_pop(L,2); // absDir, incls
}

static void emitPath(lua_State* L,int inst, BSVisitorCtx* ctx, BSBuildParam paramType, const char* field)
{
    assert( ctx->d_param != 0 );

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        // TODO: check for circular deps
        emitPath(L,config, ctx, paramType, field);
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,inst,field);
    const int path = lua_gettop(L);
    if( lua_isstring(L,path) )
    {
        if( *lua_tostring(L,path) != '/' )
        {
            // relative path
            addPath(L,absDir,path);
            lua_replace(L,path);
        }
        ctx->d_param(paramType, bs_denormalize_path(lua_tostring(L,path)),ctx->d_data);
    }

    lua_pop(L,2); // absDir, path
}

static void compilesources(lua_State* L, BSVisitorCtx* ctx, int builtins, int inlist)
{
    const int top = lua_gettop(L);

    lua_createtable(L,0,0);
    const int outlist = lua_gettop(L);
    lua_pushinteger(L,BS_ObjectFiles);
    lua_setfield(L,outlist,"#kind");
    lua_pushvalue(L,outlist);
    lua_setfield(L,PRODINST,"#out");

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,PRODINST,"to_host");
    const int to_host = lua_toboolean(L,-1);
    lua_pop(L,1);

    const BSToolchain toolchain = bs_getToolchain(L,binst,to_host);

    lua_getfield(L,binst,"#ctdefaults");
    if( to_host )
        lua_getfield(L,binst,"host_toolchain");
    else
        lua_getfield(L,binst,"target_toolchain");
    lua_rawget(L,-2);
    lua_replace(L,-2);
    const int ctdefaults = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    const int rootOutDir = lua_gettop(L);

    bs_getModuleVar(L,PRODINST,"#dir");
    const int absDir = lua_gettop(L);

    bs_getModuleVar(L,PRODINST,"#rdir");
    const int relDir = lua_gettop(L);

    const BSOperatingSystem os = bs_getOperatingSystem(L,binst,to_host);

    size_t i;

    lua_getfield(L,PRODINST,"sources");
    const int sources = lua_gettop(L);
    lua_createtable(L,lua_objlen(L,sources),0);
    const int tmp = lua_gettop(L);
    copyItems(L,inlist,tmp, BS_SourceFiles);
    int n = lua_objlen(L,tmp);

    lua_createtable(L,n,0);
    const int generated = lua_gettop(L);
    for( i = 1; i <= n; i++ )
    {
        lua_rawgeti(L,tmp,i);
        lua_rawseti(L,generated,i);
    }
    lua_setfield(L,PRODINST,"#generated");

    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        // copy all sources to a new table to avoid changing the original sources
        lua_rawgeti(L,sources,i);
        lua_rawseti(L,tmp,++n);
    }
    lua_replace(L,sources);

    // the result of source files received via dependencies appeares before the results of this source files
    copyItems(L,inlist,outlist, BS_ObjectFiles);

    n = lua_objlen(L,outlist);
    if( ctx->d_fork )
        ctx->d_fork(lua_objlen(L,sources),ctx->d_data);
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int file = lua_gettop(L);
        const int lang = bs_guessLang(lua_tostring(L,file));
        if( lang == BS_unknownLang )
            luaL_error(L,"source file type not supported: %s",lua_tostring(L,file));
        if( lang == BS_header )
        {
            lua_pop(L,1);
            continue;
        }

        if( *lua_tostring(L,file) != '/' )
            addPath(L,absDir,file); // path could be absolute!
        else
            lua_pushvalue(L,file);
        const int src = lua_gettop(L);

        addPath(L,rootOutDir,relDir);

        // we need to prefix object files of separate products in the same module
        // otherwise object files could overwrite each other
        lua_pushstring(L,"/");
        lua_getfield(L,PRODINST,"#decl");
        lua_getfield(L,-1,"#name");
        lua_replace(L,-2);

        // strip all path segments with bs_filename; otherwise subdirs have to be created for files accessed
        // from other than the module directory
#ifdef BS_HAVE_FILE_PREFIX
        // the name is actually not relevant, so we can just prefix it with a number to reduce name collisions
        // (possible when collecting  files from different directories in the same module)
        lua_pushfstring(L,"_%d_%s",i,bs_filename(lua_tostring(L,file)));
#else
        lua_pushfstring(L,"_%s",bs_filename(lua_tostring(L,file)));
#endif
        if( toolchain == BS_msvc )
            lua_pushstring(L,".obj");
        else
            lua_pushstring(L,".o");

        lua_concat(L,5); // dir, slash, prefix, filename, ext
        const int out = lua_gettop(L);

        lua_pushvalue(L,out);
        lua_rawseti(L,outlist,++n);

        switch(toolchain)
        {
        case BS_gcc:
            lua_pushstring(L,"gcc");
            break;
        case BS_clang:
            lua_pushstring(L,"clang");
            break;
        case BS_msvc:
            lua_pushstring(L,"cl");
            break;
        default:
            break;
        }
        const int cmd = lua_gettop(L);

        prefixCmd(L, cmd, binst, to_host);

        if( ctx->d_begin )
            ctx->d_begin(BS_Compile, lua_tostring(L,cmd), toolchain, os, ctx->d_data);

        lua_pop(L,1); // cmd

        if( ctx->d_param )
        {
            if( !lua_isnil(L,ctdefaults) )
                emitFlags(L,ctdefaults,ctx, BS_cflag, "cflags");
            emitFlags(L,PRODINST,ctx, BS_cflag, "cflags");

            switch(lang)
            {
            case BS_c:
                if( !lua_isnil(L,ctdefaults) )
                    emitFlags(L,ctdefaults,ctx, BS_cflag, "cflags_c");
                emitFlags(L,PRODINST,ctx, BS_cflag, "cflags_c");
                break;
            case BS_cc:
                if( !lua_isnil(L,ctdefaults) )
                    emitFlags(L,ctdefaults,ctx, BS_cflag, "cflags_cc");
                emitFlags(L,PRODINST,ctx, BS_cflag, "cflags_cc");
                break;
            case BS_objc:
                if( !lua_isnil(L,ctdefaults) )
                    emitFlags(L,ctdefaults,ctx, BS_cflag, "cflags_objc");
                emitFlags(L,PRODINST,ctx, BS_cflag, "cflags_objc");
                break;
            case BS_objcc:
                if( !lua_isnil(L,ctdefaults) )
                    emitFlags(L,ctdefaults,ctx, BS_cflag, "cflags_objcc");
                emitFlags(L,PRODINST,ctx, BS_cflag, "cflags_objcc");
                break;
            }

            if( !lua_isnil(L,ctdefaults) )
                emitFlags(L,ctdefaults,ctx, BS_define, "defines");
            emitFlags(L,PRODINST,ctx, BS_define, "defines");

            if( !lua_isnil(L,ctdefaults) )
                emitPaths(L,ctdefaults,ctx, BS_include_dir, "include_dirs");
            emitPaths(L,PRODINST,ctx, BS_include_dir, "include_dirs");

            ctx->d_param(BS_outfile, bs_denormalize_path(lua_tostring(L,out)), ctx->d_data);

            ctx->d_param(BS_infile, bs_denormalize_path(lua_tostring(L,src)), ctx->d_data);
        }

        if( ctx->d_end )
            ctx->d_end(ctx->d_data);

        lua_pop(L,3); // file, source, dest
    }
    lua_pop(L,1); // sources

    if( ctx->d_fork )
        ctx->d_fork(-1,ctx->d_data);

    lua_pop(L,6); // outlist, binst, ctdefaults, rootOutDir...relDir

    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void renderobjectfiles(lua_State* L, int list, BSVisitorCtx* ctx, int toolchain, int resKind)
{
    assert( ctx->d_param );

    // BS_ObjectFiles: list of file names
    // BS_StaticLib, BS_DynamicLib, BS_Executable: one file name
    // BS_Mixed: list of tables

    lua_getfield(L,list,"#kind");
    const int k = lua_tointeger(L,-1);
    lua_pop(L,1); // kind

    size_t i;
    switch(k)
    {
    case BS_Mixed:
        for( i = lua_objlen(L,list); i >= 1; i-- ) // turn dependency order for rendering Products in reverse order
        {
            lua_rawgeti(L,list,i);
            const int sublist = lua_gettop(L);
            assert( lua_istable(L,sublist) );
            renderobjectfiles(L,sublist,ctx, toolchain, resKind);
            lua_pop(L,1); // sublist
        }
        break;
    case BS_ObjectFiles:
        for( i = 1; i <= lua_objlen(L,list); i++ ) // keep original order
        {
            lua_rawgeti(L,list,i);
            const int path = lua_gettop(L);
            ctx->d_param(BS_infile,bs_denormalize_path(lua_tostring(L,path)),ctx->d_data);
            lua_pop(L,1); // path
        }
        break;
    case BS_StaticLib:
    case BS_DynamicLib:
        if( resKind != BS_StaticLib ) // only link libs from inlist if a dynamic lib or exe is created;
                                      // otherwise they are passed on; ar doesn't seem to create a suiable .a with .a as input
        {
            lua_rawgeti(L,list,1); // there is only one item, which has index 1
            const int path = lua_gettop(L);

            if( toolchain == BS_msvc && k == BS_DynamicLib)
            {
                // add .lib because msvc requires an import library to use the dll
                lua_pushstring(L,".lib");
                lua_concat(L,2); // the name of the import library is xyz.dll.lib
            }

            ctx->d_param(BS_infile,bs_denormalize_path(lua_tostring(L,path)),ctx->d_data);
            lua_pop(L,1); // path
        }
        break;
    default:
        // ignore
        break;
    }
}

static int makeCopyOfLibs(lua_State* L, int inlist)
{
    const int top = lua_gettop(L);
    lua_getfield(L,inlist,"#kind");
    const int k = lua_tointeger(L,-1);
    lua_pop(L,1); // kind

    if( k != BS_Mixed )
        return 0;

    size_t i, len = 0;
    len = lua_objlen(L,inlist);
    int hasLibs = 0;
    for( i = 1; i <= len; i++ )
    {
        lua_rawgeti(L,inlist,i);
        const int sublist = lua_gettop(L);
        lua_getfield(L,sublist,"#kind");
        const int k = lua_tointeger(L,-1);
        lua_pop(L,2); // kind, sublist
        assert( k != BS_Mixed );
        if( k == BS_StaticLib || k == BS_DynamicLib )
        {
            hasLibs = 1;
            break;
        }
    }
    if( hasLibs )
    {
        lua_createtable(L,0,0);
        lua_pushinteger(L,BS_Mixed);
        lua_setfield(L,-2,"#kind");
        const int outlist = lua_gettop(L);
        int n = 0;
        for( i = 1; i <= len; i++ )
        {
            lua_rawgeti(L,inlist,i);
            const int sublist = lua_gettop(L);
            lua_getfield(L,sublist,"#kind");
            const int k = lua_tointeger(L,-1);
            lua_pop(L,1); // kind
            if( k == BS_StaticLib || k == BS_DynamicLib )
                lua_rawseti(L,outlist,++n);
            else
                lua_pop(L,1); // sublist
        }
    }
    assert( top + ( hasLibs ? 1: 0 ) == lua_gettop(L) );
    return hasLibs;
}

static void link(lua_State* L, BSVisitorCtx* ctx, int builtins, int inlist, int resKind)
{
    assert( resKind == BS_Executable || resKind == BS_DynamicLib || resKind == BS_StaticLib );

    const int top = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,PRODINST,"to_host");
    const int to_host = lua_toboolean(L,-1);
    lua_pop(L,1);

    const BSToolchain toolchain = bs_getToolchain(L,binst,to_host);
    const BSOperatingSystem os = bs_getOperatingSystem(L,binst,to_host);
    const int win32 = os == BS_windows;
    const int mac = os == BS_mac;

    lua_getfield(L,binst,"#ctdefaults");
    if( to_host )
        lua_getfield(L,binst,"host_toolchain");
    else
        lua_getfield(L,binst,"target_toolchain");
    lua_rawget(L,-2);
    lua_replace(L,-2);
    const int ctdefaults = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    const int rootOutDir = lua_gettop(L);

    bs_getModuleVar(L,PRODINST,"#rdir");
    const int relDir = lua_gettop(L);

    addPath(L,rootOutDir,relDir);
    lua_pushvalue(L,-1);
    lua_pushstring(L,"/");

    if( !win32 && ( resKind == BS_DynamicLib || resKind == BS_StaticLib ) )
        lua_pushstring(L,"lib"); // if not on Windows prefix the lib name with "lib"
    else
        lua_pushstring(L,"");
    lua_getfield(L,PRODINST,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_getfield(L,PRODINST,"#decl");
        lua_getfield(L,-1,"#name");
        lua_replace(L,-2);
    }
    lua_concat(L,4);
    lua_replace(L,-2);
    const int outbase = lua_gettop(L);

    lua_pushvalue(L,outbase);
    switch(resKind)
    {
    case BS_DynamicLib:
        if( win32 )
            lua_pushstring(L,".dll");
        else if(mac)
            lua_pushstring(L,".dylib");
        else
            lua_pushstring(L,".so");
        break;
    case BS_Executable:
        if( win32 )
            lua_pushstring(L,".exe");
        else
            lua_pushstring(L,"");
        break;
    case BS_StaticLib:
        if( win32 )
            lua_pushstring(L,".lib");
        else
            lua_pushstring(L,".a");
        break;
    }
    lua_concat(L,2);
    const int outfile = lua_gettop(L);

    lua_pushvalue(L,outfile);
    lua_setfield(L,PRODINST,"#product");

    switch(toolchain)
    {
    default:
        break;
    case BS_gcc:
        switch(resKind)
        {
        case BS_Executable:
        case BS_DynamicLib:
            lua_pushstring(L, "gcc");
            break;
        case BS_StaticLib:
            lua_pushstring(L, "ar");
            break;
         }
        break;
    case BS_clang:
        switch(resKind)
        {
        case BS_Executable:
        case BS_DynamicLib:
            lua_pushstring(L, "clang");
            break;
        case BS_StaticLib:
            if( win32 )
                lua_pushstring(L, "llvm-lib");
            else
                lua_pushstring(L, "ar");
            break;
        }
        break;
    case BS_msvc:
        switch(resKind)
        {
        case BS_Executable:
        case BS_DynamicLib:
            lua_pushstring(L, "link");
            break;
        case BS_StaticLib:
            lua_pushstring(L, "lib");
            break;
        }
        break;
    }
    const int cmd = lua_gettop(L);

    prefixCmd(L, cmd, binst, to_host);

    if( ctx->d_begin )
    {
        BSBuildOperation op;
        switch(resKind)
        {
        case BS_Executable:
            op = BS_LinkExe;
            break;
        case BS_DynamicLib:
            op = BS_LinkDll;
            break;
        case BS_StaticLib:
            op = BS_LinkLib;
            break;
        }
        ctx->d_begin(op, lua_tostring(L,cmd), toolchain, os, ctx->d_data);
    }

    lua_pop(L,1); // cmd

    lua_createtable(L,0,0);
    const int outlist = lua_gettop(L);
    lua_pushinteger(L,resKind);
    lua_setfield(L,outlist,"#kind");
    lua_pushvalue(L,outfile);
    lua_rawseti(L,outlist,1);

    if( resKind == BS_StaticLib && makeCopyOfLibs(L,inlist) )
    {
        const int newOut = lua_gettop(L);
        lua_pushvalue(L,outlist);
        lua_rawseti(L,newOut,lua_objlen(L,newOut)+1);
        lua_setfield(L,PRODINST,"#out");
    }else
    {
        lua_pushvalue(L,outlist);
        lua_setfield(L,PRODINST,"#out");
    }
    lua_pop(L,1); // outlist

    if( ctx->d_param )
    {
        if( !lua_isnil(L,ctdefaults) )
            emitFlags(L,ctdefaults,ctx, BS_ldflag, "ldflags");
        emitFlags(L,PRODINST,ctx, BS_ldflag, "ldflags");

        if( !lua_isnil(L,ctdefaults) )
            emitFlags(L,ctdefaults,ctx, BS_lib_name, "lib_names");
        emitFlags(L,PRODINST,ctx, BS_lib_name, "lib_names");

        if( !lua_isnil(L,ctdefaults) )
            emitFlags(L,ctdefaults,ctx, BS_framework, "frameworks");
        emitFlags(L,PRODINST,ctx, BS_framework, "frameworks");

        if( !lua_isnil(L,ctdefaults) )
            emitPaths(L,ctdefaults,ctx, BS_lib_dir, "lib_dirs");
        emitPaths(L,PRODINST,ctx, BS_lib_dir, "lib_dirs");

        if( !lua_isnil(L,ctdefaults) )
            emitPaths(L,ctdefaults,ctx, BS_lib_file, "lib_files");
        emitPaths(L,PRODINST,ctx, BS_lib_file, "lib_files");

        lua_getfield(L,PRODINST,"def_file");
        if( !lua_isnil(L,-1) && strcmp(lua_tostring(L,-1),".") != 0 )
            emitPath(L,PRODINST,ctx, BS_defFile, "def_file");
        lua_pop(L,1);// def_file

        ctx->d_param(BS_outfile,bs_denormalize_path(lua_tostring(L,outfile)), ctx->d_data);

        renderobjectfiles(L, inlist, ctx, toolchain, resKind);
    }

    if( ctx->d_end )
        ctx->d_end(ctx->d_data);

    lua_pop(L,6); // binst, rootOutDir, relDir, ctdefaults, outbase, out
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void builddeps(lua_State* L, int inst)
{
    const int top = lua_gettop(L);

    lua_getfield(L,inst,"deps");
    const int deps = lua_gettop(L);
    if( lua_isnil(L,deps) )
    {
        lua_pop(L,1); // nil
        return;
    }

    lua_createtable(L,0,0);
    lua_pushinteger(L,BS_Mixed);
    lua_setfield(L,-2,"#kind");
    const int out = lua_gettop(L);

    const int ndeps = lua_objlen(L,deps);
    int nout = 0;
    int i;
    for( i = 1; i <= ndeps; i++ )
    {
        lua_pushcfunction(L, bs_visit);
        lua_rawgeti(L,deps,i);
        lua_pushvalue(L,CTX);
        lua_call(L,2,0);

        lua_rawgeti(L,deps,i);
        lua_getfield(L,-1,"#out");
        lua_replace(L,-2);
        // stack: subout

        const int subout = lua_gettop(L);
        int k = BS_Nothing;
        if( lua_istable(L,subout) )
        {
            lua_getfield(L,-1,"#kind");
            k = lua_tointeger(L,-1);
            lua_pop(L,1);
        }

        if( k == BS_Mixed )
        {
            const int nsubout = lua_objlen(L,subout);
            int j;
            for( j = 1; j <= nsubout; j++ )
            {
                lua_rawgeti(L,subout,j);

                lua_getfield(L,-1,"#kind");
                const int kk = lua_tointeger(L,-1);
                lua_pop(L,1);
                assert( kk != BS_Mixed);

                lua_rawseti(L,out,++nout);
            }
            lua_pop(L,1); // subout
        }else if( lua_istable(L,subout) )
            lua_rawseti(L,out,++nout); // eats subout
        else
            lua_pop(L,1);
    }
    lua_setfield(L,inst,"#out");
    lua_pop(L,1); // deps
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void library(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);
    lua_getfield(L,PRODINST,"#out");
    const int inlist = lua_gettop(L); // inlist is of kind BS_Mixed and doesn't have items of kind BS_Mixed
    assert( lua_istable(L,inlist) );
    compilesources(L,ctx,builtins, inlist);

    lua_getfield(L,PRODINST,"lib_type");
    const int lib_type = ( strcmp(lua_tostring(L,-1),"shared") == 0 ? BS_DynamicLib : BS_StaticLib );
    lua_pop(L,1); // libtype

    lua_getfield(L,PRODINST,"#out");
    // compilerOut includes the object files from inlist and the ones generated by compilesources
    const int compilerOut = lua_gettop(L);
    if( makeCopyOfLibs(L,inlist) )
    {
        // top of stack is a new BS_Mixed created by makeCopyOfLibs which includes the libs from inlist (and only those)
        // store the result of makeCopyOfLibs to the inlist slot
        lua_replace(L,inlist);
        // now consume compilerOut and add it to inlist
        lua_rawseti(L,inlist,lua_objlen(L,inlist)+1);
        // now the new BS_Mixed also includes the BS_ObjectFiles from compile output
    }else
    {
        // make BS_ObjectFiles from compile output the new inlist
        // now consume compilerOut and make it the new inlist
        lua_replace(L,inlist);
    }

    // inlist here includes the object files generated by compilesources and inherited from the initial inlist
    // in case a dynamic lib is to be generated by link(), inlist also includes the libs inherited from the initial inlist

    // link sets out to a new table of kind BS_DynamicLib or BS_StaticLib; inlist is not passed out
    link(L,ctx,builtins,inlist,lib_type);

    lua_pop(L,1); // inlist
    assert( top == lua_gettop(L) );

    // passes on one lib (either BS_DynamicLib or BS_StaticLib), or a BS_Mixed of libs
}

static void executable(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);
    lua_getfield(L,PRODINST,"#out");
    const int inlist = lua_gettop(L);
    assert( lua_istable(L,inlist) );
    compilesources(L,ctx,builtins,inlist);

    lua_getfield(L,PRODINST,"#out");
    if( makeCopyOfLibs(L,inlist) )
    {
        // inlist included libs, so top of stack is new BS_Mixed which includes these libs (and only those)
        lua_replace(L,inlist);
        lua_rawseti(L,inlist,lua_objlen(L,inlist)+1);
        // now the new BS_Mixed also includes the BS_ObjectFiles from compile output
    }else
        lua_replace(L,inlist); // make BS_ObjectFiles from compile output the new inlist

    link(L,ctx,builtins,inlist,BS_Executable);
    lua_pop(L,1); // mixed
    assert( top == lua_gettop(L) );

    // passes on one BS_Executable
}

static void sourceset(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);
    lua_getfield(L,PRODINST,"#out");
    const int inlist = lua_gettop(L);
    assert( lua_istable(L,inlist) );
    compilesources(L,ctx,builtins,inlist);
    // #out is now a BS_ObjectFiles

    if( makeCopyOfLibs(L,inlist) )
    {
        // inlist included libs, so top of stack is new BS_Mixed which includes these libs (and only those)
        lua_replace(L,inlist);
        lua_getfield(L,PRODINST,"#out");
        lua_rawseti(L,inlist,lua_objlen(L,inlist)+1); // add BS_ObjectFiles to the new BS_Mixed
        lua_setfield(L,PRODINST,"#out"); // set mixed again as inst.#out
    }else
        lua_pop(L,1); // inlist

    // passes on a BS_ObjectFiles or a BS_Mixed with BS_ObjectFiles and the libs from orig inlist

    assert( top == lua_gettop(L) );
}

static void group(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    // NOP. deps were already built and result handed to inst.#out
}

static void config(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    // NOP
}

static void concatReplace(lua_State* L, int to, const char* from, int fromLen)
{
    lua_pushvalue(L,to);
    lua_pushlstring(L,from,fromLen);
    lua_concat(L,2);
    lua_replace(L,to);
}

static BSPathStatus apply_arg_expansion(lua_State* L,int inst, int builtins, const char* source, const char* string)
{
    const char* s = string;
    lua_pushstring(L,"");
    const int out = lua_gettop(L);
    while( *s )
    {
        int offset, len;
        const BSPathStatus res = bs_findToken( s, &offset, &len );
        if( res == BS_OK )
        {
            concatReplace(L,out,s,offset);

            const char* start = s + offset + 2; // skip {{
            const BSPathPart t = bs_tokenType(start, len - 4);
            if( t == BS_NoPathPart )
                return BS_NotSupported;
            if( t <= BS_extension )
            {
                if( source )
                {
                    int len2;
                    const char* val = bs_path_part(source,t,&len2);
                    if( len2 < 0 )
                        return BS_NotSupported;
                    concatReplace(L,out,val,len2);
                }else
                    return BS_NotSupported;
            }else if( t == BS_RootBuildDir || t == BS_CurBuildDir )
            {
                lua_getfield(L,builtins,"#inst");
                lua_getfield(L,-1,"root_build_dir");
                lua_replace(L,-2);
                if( t == BS_RootBuildDir )
                {
                    const char* str = bs_denormalize_path(lua_tostring(L,-1));
                    concatReplace(L,out,str,strlen(str));
                    lua_pop(L,1);
                }else
                {
                    bs_getModuleVar(L,inst,"#rdir");
                    addPath(L,-2,-1); // root_build_dir, rdir, root_build_dir+rdir
                    const char* str = bs_denormalize_path(lua_tostring(L,-1));
                    concatReplace(L,out,str,strlen(str));
                    lua_pop(L,3);
                }
            }
            s += offset + len;
        }else if( res == BS_NOP )
        {
            // copy rest of string
            len = strlen(s);
            concatReplace(L,out,s,len);
            s += len;
        }else
            return res;
    }
    return BS_OK;
}

static void callLua(lua_State* L, BSVisitorCtx* ctx, int builtins, int inst, int app, int script, const char* source)
{
    const int top = lua_gettop(L);

    if( ctx->d_begin )
        ctx->d_begin(BS_RunLua, bs_denormalize_path(lua_tostring(L,app)), BS_notc, BS_noos, ctx->d_data);

    if( ctx->d_param )
    {
        lua_getfield(L,inst,"args");
        const int arglist = lua_gettop(L);
        size_t j;
        for( j = 1; j <= lua_objlen(L,arglist); j++ )
        {
            lua_rawgeti(L,arglist,j);
            const int arg = lua_gettop(L);
            if( apply_arg_expansion(L,inst,builtins,source,lua_tostring(L,arg)) != BS_OK )
                luaL_error(L,"cannot do source expansion, invalid placeholders in string: %s", lua_tostring(L,-1));
            lua_replace(L,arg);
            ctx->d_param(BS_arg, lua_tostring(L,arg), ctx->d_data);
            lua_pop(L,1); // arg
        }
        lua_pop(L,1); // arglist

        ctx->d_param(BS_infile, bs_denormalize_path(lua_tostring(L,script)), ctx->d_data);
    }

    if( ctx->d_end )
        ctx->d_end(ctx->d_data);

    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void script(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);

    lua_createtable(L,0,0);
    const int out = lua_gettop(L);
    lua_pushinteger(L,BS_SourceFiles);
    lua_setfield(L,out,"#kind");
    lua_pushvalue(L,out);
    lua_setfield(L,PRODINST,"#out");

    bs_getModuleVar(L,PRODINST,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    lua_getfield(L,-1,"root_build_dir");
    lua_replace(L,-2);
    bs_getModuleVar(L,PRODINST,"#rdir");
    addPath(L,-2,-1); // root_build_dir, rdir, root_build_dir+rdir
    lua_replace(L,-3);
    lua_pop(L,1);
    const int outDir = lua_gettop(L);

    lua_getfield(L,PRODINST,"outputs");
    const int outputs = lua_gettop(L);
    size_t j;
    for( j = 1; j <= lua_objlen(L,outputs); j++ )
    {
        lua_rawgeti(L,outputs,j);
        const int src = lua_gettop(L);

        if( *lua_tostring(L,src) != '/' )
        {
            addPath(L,outDir,src);
            lua_replace(L,src);
        }else
            luaL_error(L,"the 'outputs' field requires relative paths");

        lua_rawseti(L,out,j);
    }
    lua_pop(L,2); // outDir, outputs


    lua_getfield(L,PRODINST,"script");
    const int script = lua_gettop(L);
    if( *lua_tostring(L,script) != '/' )
    {
        // relative path
        addPath(L,absDir,script);
        lua_replace(L,script);
    }

    bs_thisapp2(L);
    const int app = lua_gettop(L);

    callLua(L,ctx,builtins,PRODINST,app,script,0);

    lua_pop(L,4); // out, abDir, script, app

    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void runforeach(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);

    lua_pushnil(L);
    lua_setfield(L,PRODINST,"#out");

    bs_getModuleVar(L,PRODINST,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,PRODINST,"script");
    const int script = lua_gettop(L);
    if( *lua_tostring(L,script) != '/' )
    {
        // relative path
        addPath(L,absDir,script);
        lua_replace(L,script);
    }

    bs_thisapp2(L);
    const int app = lua_gettop(L);

    size_t i;
    lua_getfield(L,PRODINST,"sources");
    const int sources = lua_gettop(L);
    if( ctx->d_fork )
        ctx->d_fork( lua_objlen(L,sources), ctx->d_data );
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int source = lua_gettop(L);
        if( *lua_tostring(L,source) != '/' )
        {
            addPath(L,absDir,source);
            lua_replace(L,source);
        }

        callLua(L,ctx, builtins,PRODINST,app,script,lua_tostring(L,source));

        lua_pop(L,1); // source
    }
    if( ctx->d_fork )
        ctx->d_fork( -1, ctx->d_data );

    lua_pop(L,4); // abDir, script, app, sources
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void runmoc(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);

    lua_createtable(L,0,0);
    const int outlist = lua_gettop(L);
    lua_pushinteger(L,BS_SourceFiles);
    lua_setfield(L,outlist,"#kind");
    lua_pushvalue(L,outlist);
    lua_setfield(L,PRODINST,"#out");

    bs_getModuleVar(L,PRODINST,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    bs_getModuleVar(L,PRODINST,"#rdir");
    addPath(L,-2,-1);
    lua_replace(L,-3);
    lua_pop(L,1);
    const int outDir = lua_gettop(L);

    lua_getfield(L,PRODINST,"tool_dir");
    const int mocPath = lua_gettop(L);
    if( lua_isnil(L,mocPath) || strcmp(".",lua_tostring(L,mocPath)) == 0 )
    {
        lua_getfield(L,binst,"moc_path");
        lua_replace(L,mocPath);
    }
    if( lua_isnil(L,mocPath) || strcmp(".",lua_tostring(L,mocPath)) == 0 )
    {
        lua_pushstring(L,"moc");
        lua_replace(L,mocPath);
    }else if( *lua_tostring(L,mocPath) != '/' )
        luaL_error(L,"moc_path cannot be relative: %s", lua_tostring(L,mocPath));
    else
    {
        lua_pushfstring(L,"%s/moc", lua_tostring(L,mocPath));
        lua_replace(L,mocPath);
    }

    lua_getfield(L,PRODINST,"sources");
    const int sources = lua_gettop(L);
    int n = 0;
    size_t i;
    if( ctx->d_fork )
        ctx->d_fork( lua_objlen(L,sources), ctx->d_data );
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int source = lua_gettop(L);
        const int lang = bs_guessLang(lua_tostring(L,source));

        if( *lua_tostring(L,source) != '/' )
        {
            addPath(L,absDir,source);
            lua_replace(L,source);
        }

        lua_getfield(L,PRODINST,"defines");
        const int defs = lua_gettop(L);

        int len;
        const char* name = bs_path_part(lua_tostring(L,source),BS_baseName,&len);
        lua_pushlstring(L,name,len);
        if( lang == BS_header )
            // this file is automatically passed to the compiler over the deps chain; the user doesn't see it
            lua_pushfstring(L,"%s/moc_%s.cpp",lua_tostring(L,outDir), lua_tostring(L,-1));
        else
            // this file has to be included at the bottom of the cpp file, so use the naming of the Qt documentation.
            lua_pushfstring(L,"%s/%s.moc",lua_tostring(L,outDir), lua_tostring(L,-1));
        lua_replace(L,-2);
        const int outFile = lua_gettop(L);

        if( ctx->d_begin )
            ctx->d_begin(BS_RunMoc, bs_denormalize_path(lua_tostring(L,mocPath)), BS_notc, BS_noos, ctx->d_data);

        if( ctx->d_param )
        {
            ctx->d_param(BS_infile, bs_denormalize_path(lua_tostring(L,source)), ctx->d_data);
            ctx->d_param(BS_outfile, bs_denormalize_path(lua_tostring(L,outFile)), ctx->d_data);

            size_t j;
            const size_t numOfDefs = lua_objlen(L,defs);
            for( j = 1; j <= numOfDefs; j++ )
            {
                lua_rawgeti(L,defs,j);
                ctx->d_param(BS_define, lua_tostring(L,-1), ctx->d_data);
                lua_pop(L,1); // define
            }
        }

        if( ctx->d_end )
            ctx->d_end(ctx->d_data);

        if( lang == BS_header )
        {
            lua_pushvalue(L,outFile);
            lua_rawseti(L,outlist,++n);
        }

        lua_pop(L,3); // source, defs, outFile
    }
    if( ctx->d_fork )
        ctx->d_fork( -1, ctx->d_data );

    lua_pop(L,6); // outlist absDir binst outDir mocPath sources
    const int bottom = lua_gettop(L);
    assert( top == bottom );

    // passes on one BS_SourceFiles
}

static void runrcc(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);

    lua_createtable(L,0,0);
    const int outlist = lua_gettop(L);
    lua_pushinteger(L,BS_SourceFiles);
    lua_setfield(L,outlist,"#kind");
    lua_pushvalue(L,outlist);
    lua_setfield(L,PRODINST,"#out");

    bs_getModuleVar(L,PRODINST,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    bs_getModuleVar(L,PRODINST,"#rdir");
    addPath(L,-2,-1);
    lua_replace(L,-3);
    lua_pop(L,1);
    const int outDir = lua_gettop(L);

    lua_getfield(L,PRODINST,"tool_dir");
    const int app = lua_gettop(L);
    if( lua_isnil(L,app) || strcmp(".",lua_tostring(L,app)) == 0 )
    {
        lua_getfield(L,binst,"rcc_path");
        lua_replace(L,app);
    }
    if( lua_isnil(L,app) || strcmp(".",lua_tostring(L,app)) == 0 )
    {
        lua_pushstring(L,"rcc");
        lua_replace(L,app);
    }else if( *lua_tostring(L,app) != '/' )
        luaL_error(L,"rcc_path cannot be relative: %s", lua_tostring(L,app));
    else
    {
        lua_pushfstring(L,"%s/rcc", lua_tostring(L,app));
        lua_replace(L,app);
    }

    size_t i;
    lua_getfield(L,PRODINST,"sources");
    const int sources = lua_gettop(L);
    if( ctx->d_fork )
        ctx->d_fork( lua_objlen(L,sources), ctx->d_data );
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int source = lua_gettop(L);
        if( *lua_tostring(L,source) != '/' )
        {
            addPath(L,absDir,source);
            lua_replace(L,source);
        }

        lua_pushfstring(L,"%s/qrc_%s.cpp",lua_tostring(L,outDir), bs_filename(lua_tostring(L,source)));
        const int outFile = lua_gettop(L);

        lua_pushvalue(L,outFile);
        lua_rawseti(L,outlist,i);

        if( ctx->d_begin )
            ctx->d_begin(BS_RunRcc, bs_denormalize_path(lua_tostring(L,app)), BS_notc, BS_noos, ctx->d_data);

        if( ctx->d_param )
        {
            int len = 0;
            const char* name = bs_path_part(lua_tostring(L,source),BS_baseName, &len);
            lua_pushlstring(L,name,len);
            ctx->d_param(BS_name, bs_denormalize_path(lua_tostring(L,-1)), ctx->d_data);
            lua_pop(L,1);
            ctx->d_param(BS_infile, bs_denormalize_path(lua_tostring(L,source)), ctx->d_data);
            ctx->d_param(BS_outfile, bs_denormalize_path(lua_tostring(L,outFile)), ctx->d_data);
        }

        if( ctx->d_end )
            ctx->d_end(ctx->d_data);

        lua_pop(L,2); // source, outFile
    }
    if( ctx->d_fork )
        ctx->d_fork( -1, ctx->d_data );

    lua_pop(L,6); // outlist absDir binst outDir app sources
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void runuic(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);

    lua_createtable(L,0,0);
    const int outlist = lua_gettop(L);
    lua_pushinteger(L,BS_SourceFiles);
    lua_setfield(L,outlist,"#kind");
    lua_pushvalue(L,outlist);
    lua_setfield(L,PRODINST,"#out");

    bs_getModuleVar(L,PRODINST,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    bs_getModuleVar(L,PRODINST,"#rdir");
    addPath(L,-2,-1);
    lua_replace(L,-3);
    lua_pop(L,1);
    const int outDir = lua_gettop(L);

    lua_getfield(L,PRODINST,"tool_dir");
    const int app = lua_gettop(L);
    if( lua_isnil(L,app) || strcmp(".",lua_tostring(L,app)) == 0 )
    {
        lua_getfield(L,binst,"uic_path");
        lua_replace(L,app);
    }
    if( lua_isnil(L,app) || strcmp(".",lua_tostring(L,app)) == 0 )
    {
        lua_pushstring(L,"uic");
        lua_replace(L,app);
    }else if( *lua_tostring(L,app) != '/' )
        luaL_error(L,"uic_path cannot be relative: %s", lua_tostring(L,app));
    else
    {
        lua_pushfstring(L,"%s/uic", lua_tostring(L,app));
        lua_replace(L,app);
    }

    size_t i;
    lua_getfield(L,PRODINST,"sources");
    const int sources = lua_gettop(L);
    if( ctx->d_fork )
        ctx->d_fork( lua_objlen(L,sources), ctx->d_data );
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int source = lua_gettop(L);
        if( *lua_tostring(L,source) != '/' )
        {
            addPath(L,absDir,source);
            lua_replace(L,source);
        }

        int len = 0;
        const char* name = bs_path_part(lua_tostring(L,source),BS_baseName, &len);
        lua_pushlstring(L,name,len);
        lua_pushfstring(L,"%s/ui_%s.h",lua_tostring(L,outDir), lua_tostring(L,-1));
        lua_replace(L,-2);
        const int outFile = lua_gettop(L);

        if( ctx->d_begin )
            ctx->d_begin(BS_RunUic, bs_denormalize_path(lua_tostring(L,app)), BS_notc, BS_noos, ctx->d_data);

        if( ctx->d_param )
        {
            ctx->d_param(BS_infile, bs_denormalize_path(lua_tostring(L,source)), ctx->d_data);
            ctx->d_param(BS_outfile, bs_denormalize_path(lua_tostring(L,outFile)), ctx->d_data);
        }

        if( ctx->d_end )
            ctx->d_end(ctx->d_data);

        lua_pop(L,2); // source, outFile
    }
    if( ctx->d_fork )
        ctx->d_fork( -1, ctx->d_data );

    lua_pop(L,6); // outlist absDir binst outDir app sources
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void copy(lua_State* L,BSVisitorCtx* ctx, int cls, int builtins)
{
    const int top = lua_gettop(L);

    lua_getfield(L,PRODINST,"#out");
    const int inlist = lua_gettop(L); // inlist is of kind BS_Mixed and doesn't have items of kind BS_Mixed
    assert( lua_istable(L,inlist) );

    lua_pushnil(L);
    lua_setfield(L,PRODINST,"#out");

    bs_getModuleVar(L,PRODINST,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    lua_getfield(L,-1,"root_build_dir");
    lua_replace(L,-2);
    const int outDir = lua_gettop(L);

    size_t i;

    lua_getfield(L,PRODINST,"sources");
    const int sources = lua_gettop(L);
    lua_createtable(L,lua_objlen(L,sources),0);
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        // copy all sources to a new table to avoid changing the original sources
        lua_rawgeti(L,sources,i);
        lua_rawseti(L,-2,i);
    }
    lua_replace(L,-2);

    lua_getfield(L,PRODINST,"use_deps");
    const int use_deps = lua_gettop(L);
    assert(lua_istable(L,use_deps));
    for( i = 1; i <= lua_objlen(L,use_deps); i++ )
    {
        lua_rawgeti(L,use_deps,i);
        const char* what = lua_tostring(L,-1);
        if( strcmp(what,"object_file") == 0 )
            copyItems(L,inlist,sources, BS_ObjectFiles);
        else if( strcmp(what,"source_file") == 0 )
            copyItems(L,inlist,sources, BS_SourceFiles);
        else if( strcmp(what,"static_lib") == 0 )
            copyItems(L,inlist,sources, BS_StaticLib);
        else if( strcmp(what,"shared_lib") == 0 )
            copyItems(L,inlist,sources, BS_DynamicLib);
        else if( strcmp(what,"executable") == 0 )
            copyItems(L,inlist,sources, BS_Executable);
        else
            assert(0);
        lua_pop(L,1); // use_deps item
    }
    lua_pop(L,1); // use_deps

    lua_getfield(L,PRODINST,"outputs");
    const int outputs = lua_gettop(L);
    const size_t len = lua_objlen(L,outputs);
    if( len == 0 )
    {
        lua_getfield(L,PRODINST,"#decl");
        calcdesig(L,-1);
        luaL_error(L,"outputs in Copy instance '%s' cannot be empty", lua_tostring(L,-1));
    }

    if( ctx->d_fork )
        ctx->d_fork( lua_objlen(L,sources), ctx->d_data );

    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int from = lua_gettop(L);
        if( *lua_tostring(L,from) != '/' )
        {
            addPath(L,absDir,from);
            lua_replace(L,from);
        }

        size_t j;
        for( j = 1; j <= len; j++ )
        {
            lua_rawgeti(L,outputs,j);
            const int to = lua_gettop(L);

            if( bs_apply_source_expansion(lua_tostring(L,from),lua_tostring(L,to), 1) != BS_OK )
                luaL_error(L,"cannot do source expansion, invalid placeholders in path: %s", lua_tostring(L,to));
            lua_pushstring(L,bs_global_buffer());
            lua_replace(L,to);
            if( *lua_tostring(L,to) != '/' )
            {
                addPath(L,outDir,to);
                lua_replace(L,to);
            }else
            {
                lua_getfield(L,PRODINST,"#decl");
                calcdesig(L,-1);
                luaL_error(L,"outputs in Copy instance '%s' require relative paths", lua_tostring(L,-1));
            }

            if( ctx->d_begin )
                ctx->d_begin(BS_Copy, "copy", BS_notc, BS_noos, ctx->d_data);

            if( ctx->d_param )
            {
                ctx->d_param(BS_infile, bs_denormalize_path(lua_tostring(L,from)), ctx->d_data);
                ctx->d_param(BS_outfile, bs_denormalize_path(lua_tostring(L,to)), ctx->d_data);
            }

            if( ctx->d_end )
                ctx->d_end(ctx->d_data);

            lua_pop(L,1); // to
        }
        lua_pop(L,1); // from
    }
    if( ctx->d_fork )
        ctx->d_fork( -1, ctx->d_data );

    lua_pop(L,5); // inlist, absDir, OutDir, sources, outputs
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void message(lua_State* L,BSVisitorCtx* ctx, int precheck)
{
    const int top = lua_gettop(L);

    lua_getfield(L,PRODINST,"msg_type");
    const int msg_type = lua_gettop(L);

    assert( ctx->d_log );

    BSRowCol loc;
    loc.row = 0;
    loc.col = 0;
    const char* label = 0;

    va_list args;
    va_end(args);

    lua_getfield(L,PRODINST,"text");
    if( strcmp(lua_tostring(L,msg_type),"error") == 0 )
    {
        ctx->d_log(BS_Error, ctx->d_data, label, loc, lua_tostring(L,-1), args);
        lua_pushnil(L);
        lua_error(L);
    }else if( strcmp(lua_tostring(L,msg_type),"warning") == 0 && !precheck )
    {
        ctx->d_log(BS_Warning, ctx->d_data, label, loc, lua_tostring(L,-1), args);
    }else if( !precheck )
    {
        ctx->d_log(BS_Message, ctx->d_data, label, loc, lua_tostring(L,-1), args);
    }
    lua_pop(L,2); // msg_type, text

    assert( top == lua_gettop(L) );
}

int bs_visit(lua_State* L)
{
    const int top = lua_gettop(L);

    lua_getfield(L,PRODINST,"#out");
    const int built = !lua_isnil(L,-1);
    lua_pop(L,1);
    if( built )
        return 1; // we're already built

    BSVisitorCtx* ctx = (BSVisitorCtx*)lua_topointer(L,CTX);

    if( ctx->d_log == 0 )
        ctx->d_log = bs_defaultLogger;

    lua_getmetatable(L,PRODINST);
    const int cls = lua_gettop(L);

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getfield(L,cls,"#name");
    const char* clsName = lua_tostring(L,-1);

    builddeps(L,PRODINST);

    if( ctx->d_begin )
    {
        lua_getfield(L,PRODINST,"#decl");
        calcdesig(L,-1);
        ctx->d_begin(BS_EnteringProduct,lua_tostring(L,-1),0,0, ctx->d_data);
        lua_pop(L,2); // decl, desig
    }

    // use isa instead of strcmp so that users can subclass the built-in classes
    if( isa( L, builtins, cls, "Library" ) )
        library(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "Executable") )
        executable(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "SourceSet") )
        sourceset(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "Group") )
        group(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "Config") )
        config(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "LuaScript") )
        script(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "LuaScriptForeach") )
        runforeach(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "Copy") )
        copy(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "Message") )
        message(L,ctx,0);
    else if( isa( L, builtins, cls, "Moc") )
        runmoc(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "Rcc") )
        runrcc(L,ctx,cls,builtins);
    else if( isa( L, builtins, cls, "Uic") )
        runuic(L,ctx,cls,builtins);
    else
        luaL_error(L,"don't know how to build instances of class '%s'", clsName);

    lua_pop(L,3); // cls, builtins, clsName

    assert( top == lua_gettop(L) );
    return 0; // inst
}

int bs_resetOut(lua_State* L)
{
    enum { MODEF = 1 };

    const int top = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,MODEF); i++ )
    {
        lua_rawgeti(L,MODEF,i);
        const int sub = lua_gettop(L);

        lua_getfield(L,sub,"#kind");
        const int k = lua_tointeger(L,-1);
        lua_pop(L,1);

        if( k == BS_ModuleDef )
        {
            lua_pushcfunction(L,bs_resetOut);
            lua_pushvalue(L,sub);
            lua_call(L,1,0);
        }else if( k == BS_VarDecl )
        {
            lua_getfield(L,sub,"#inst");
            const int PRODINST = lua_gettop(L);
            if( lua_istable(L,PRODINST) )
            {
                lua_pushnil(L);
                lua_setfield(L,PRODINST,"#out");
            }
            lua_pop(L,1); // PRODINST
        }

        lua_pop(L,1); // sub
    }
    assert( top == lua_gettop(L) );
    return 0;
}

BSVisitorCtx*bs_newctx(lua_State* L)
{
    BSVisitorCtx* ctx = (BSVisitorCtx*)lua_newuserdata(L, sizeof(BSVisitorCtx) );
    memset(ctx,0,sizeof(BSVisitorCtx));
    return ctx;
}
