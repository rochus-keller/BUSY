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

#include "bsqmakegen.h"
#include "bsrunner.h"
#include "bshost.h"
#include "bsparser.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "lauxlib.h"

static const char* s_listFill1 = " \\\n\t\"";

static int mark(lua_State* L) // args: productinst, no returns
{
    const int inst = 1;

    const int top = lua_gettop(L);

    lua_getfield(L,inst,"#decl");
    const int decl = lua_gettop(L);

    lua_getfield(L,-1,"#qmake");
    if( lua_isnil(L,-1) )
    {
        lua_pushinteger(L,0);
        lua_setfield(L,decl,"#qmake"); // mark decl
#if 0
        bs_declpath(L,decl,".");
        fprintf(stdout,"mark decl: %s\n", lua_tostring(L,-1)); // TEST
        lua_pop(L,1);
#endif
    }
    lua_pop(L,1); // qmake

    lua_getfield(L,decl,"#owner");
    lua_replace(L,decl);
    while( lua_istable(L,decl) )
    {
        // mark all modules up to top-level
        lua_getfield(L,decl,"#qmake");
        if( lua_isnil(L,-1) )
        {
            lua_pushinteger(L,0);
            lua_setfield(L,decl,"#qmake");
        }
        lua_pop(L,1); // qmake
        lua_getfield(L,decl,"^");
        lua_replace(L,decl);
    }
    lua_pop(L,1); // decl

    lua_getfield(L,inst,"deps");
    const int deps = lua_gettop(L);
    if( lua_isnil(L,deps) )
        return 0;

    const int ndeps = lua_objlen(L,deps);
    int i;
    for( i = 1; i <= ndeps; i++ )
    {
        // move along dependency tree and mark
        lua_pushcfunction(L, mark);
        lua_rawgeti(L,deps,i);
        lua_call(L,1,0);
    }
    lua_pop(L,1); // deps

    const int bottom = lua_gettop(L);
    assert( top == bottom );

    return 0;
}

static void addPath(lua_State* L, int lhs, int rhs)
{
    if( *lua_tostring(L,rhs) == '/' )
        lua_pushvalue(L, rhs);
    else if( bs_add_path(L,lhs,rhs) )
        luaL_error(L,"creating absolute path from provided root gives an error: %s %s",
                   lua_tostring(L,lhs), lua_tostring(L,rhs) );
}

static int isa(lua_State* L, int builtins, int cls, const char* what )
{
    lua_getfield(L,builtins,what);
    const int res = bs_isa(L,-1,cls);
    lua_pop(L,1);
    return res;
}

static void visitDeps(lua_State* L, int inst);

static void addDep(lua_State* L, int list, int kind, int path )
{
    lua_createtable(L,0,2);
    const int t = lua_gettop(L);
    lua_pushinteger(L,kind);
    lua_setfield(L,t,"#kind");
    lua_pushvalue(L,path);
    lua_setfield(L,t,"#path");
    lua_rawseti(L,list, lua_objlen(L,list)+1); // eats t
}

static void pushExecutableName(lua_State* L, int inst, int builtins)
{
    const int top = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"target_os");
    const int win32 = strcmp(lua_tostring(L,-1),"win32") == 0 || strcmp(lua_tostring(L,-1),"winrt") == 0;
    lua_pop(L,2); // target_os, binst

    lua_getfield(L,inst,"#decl");
    lua_getfield(L,-1,"#name");
    lua_replace(L,-2);
    const int declname = lua_gettop(L);

    bs_getModuleVar(L,inst,"#rdir");
    const int relDir = lua_gettop(L);

    lua_pushstring(L,"$$root_build_dir");
    lua_pushstring(L,lua_tostring(L,relDir)+1); // remove '.' from './'
    lua_pushfstring(L,"/%s/", lua_tostring(L,declname));
    lua_getfield(L,inst,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_pushvalue(L,declname);
    }

    if( win32 )
        lua_pushstring(L,".exe");
    else
        lua_pushstring(L,"");

    lua_concat(L,5);

    lua_replace(L,declname);

    lua_pop(L,1); // relDir

    assert( top + 1 == lua_gettop(L) );
}

static int pushLibraryName(lua_State* L, int inst, int builtins, int forceStatic)
{
    const int top = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"target_os");
    const int win32 = strcmp(lua_tostring(L,-1),"win32") == 0 || strcmp(lua_tostring(L,-1),"winrt") == 0;
    const int mac = strcmp(lua_tostring(L,-1),"darwin") == 0 || strcmp(lua_tostring(L,-1),"macos") == 0;
    lua_pop(L,2); // target_os, binst

    lua_getfield(L,inst,"lib_type");
    const char* str = lua_tostring(L,-1);
    const int lib_type = forceStatic || str == 0 ? BS_StaticLib :
                                       strcmp(str,"shared") == 0 ? BS_DynamicLib : BS_StaticLib;
    lua_pop(L,1);

    lua_getfield(L,inst,"#decl");
    lua_getfield(L,-1,"#name");
    lua_replace(L,-2);
    const int declname = lua_gettop(L);

    bs_getModuleVar(L,inst,"#rdir");
    const int relDir = lua_gettop(L);

    lua_pushstring(L,"$$root_build_dir");
    lua_pushstring(L,lua_tostring(L,relDir)+1); // remove '.' from './'
    lua_pushfstring(L,"/%s/lib", lua_tostring(L,declname));
    lua_getfield(L,inst,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_pushvalue(L,declname);
    }

    if( lib_type == BS_DynamicLib )
    {
        if( win32 )
            lua_pushstring(L,".dll");
        else if(mac)
            lua_pushstring(L,".dylib");
        else
            lua_pushstring(L,".so");
    }else
    {
        if( win32 )
            lua_pushstring(L,".lib");
        else
            lua_pushstring(L,".a");
    }
    lua_concat(L,5);

    lua_replace(L,declname);

    lua_pop(L,1); // relDir

    assert( top + 1 == lua_gettop(L) );
    return lib_type;
}

static void libraryDep(lua_State* L, int inst, int builtins, int forceStatic)
{
    const int top = lua_gettop(L);
    visitDeps(L,inst);

#if 0
    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"target_os");
    const int win32 = strcmp(lua_tostring(L,-1),"win32") == 0 || strcmp(lua_tostring(L,-1),"winrt") == 0;
    const int mac = strcmp(lua_tostring(L,-1),"darwin") == 0 || strcmp(lua_tostring(L,-1),"macos") == 0;
    lua_pop(L,1);

    lua_getfield(L,inst,"lib_type");
    const int lib_type = forceStatic ? BS_StaticLib :
                                       strcmp(lua_tostring(L,-1),"shared") == 0 ? BS_DynamicLib : BS_StaticLib;
    lua_pop(L,1);

    lua_getfield(L,inst,"#decl");
    lua_getfield(L,-1,"#name");
    lua_replace(L,-2);
    const int declname = lua_gettop(L);

    bs_getModuleVar(L,inst,"#rdir");
    const int relDir = lua_gettop(L);

    lua_pushstring(L,"$$root_build_dir");
    lua_pushstring(L,lua_tostring(L,relDir)+1); // remove '.' from './'
    lua_pushfstring(L,"/%s/lib", lua_tostring(L,declname));
    lua_getfield(L,inst,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_pushvalue(L,declname);
    }

    if( lib_type == BS_DynamicLib )
    {
        if( win32 )
            lua_pushstring(L,".dll");
        else if(mac)
            lua_pushstring(L,".dylib");
        else
            lua_pushstring(L,".so");
    }else
    {
        if( win32 )
            lua_pushstring(L,".lib");
        else
            lua_pushstring(L,".a");
    }
    lua_concat(L,5);
    const int path = lua_gettop(L);

    lua_getfield(L,inst,"#out");
    const int out = lua_gettop(L);
    if( lua_isnil(L,out) )
    {
        lua_createtable(L,0,0);
        lua_replace(L,out);
        lua_pushvalue(L,out);
        lua_setfield(L,inst,"#out");
    }

    addDep(L,out,lib_type,path);

    lua_pop(L,5); // binst, path, declname, relDir, out
#else
    const int lib_type = pushLibraryName(L,inst,builtins,forceStatic);
    const int path = lua_gettop(L);
    const char* str = lua_tostring(L,path);

    lua_getfield(L,inst,"#out");
    const int out = lua_gettop(L);
    if( lua_isnil(L,out) )
    {
        lua_createtable(L,0,0);
        lua_replace(L,out);
        lua_pushvalue(L,out);
        lua_setfield(L,inst,"#out");
    }
    addDep(L,out,lib_type,path);

    lua_pop(L,2); // path, out
#endif

    assert( top == lua_gettop(L) );
}

static void mergeOut(lua_State* L, int toInst, int fromInst )
{
    lua_getfield(L,fromInst,"#out");
    if( lua_isnil(L,-1) )
    {
        lua_pop(L,1);
        return;
    }
    const int fromOut = lua_gettop(L);

    lua_getfield(L,toInst,"#out");
    if( lua_isnil(L,-1) )
    {
        lua_pop(L,1);
        lua_createtable(L,0,0);
        lua_pushvalue(L,-1);
        lua_setfield(L,toInst,"#out");
    }
    const int toOut = lua_gettop(L);

    // iterate over dep and copy everything to out
    size_t i;
    for( i = 1; i <= lua_objlen(L,fromOut); i++ )
    {
        lua_rawgeti(L, fromOut, i);
        lua_rawseti(L,toOut, lua_objlen(L,toOut) + 1);
    }

    lua_pop(L,2); // fromOut, toOut
}

static void groupDep(lua_State* L, int inst)
{
    visitDeps(L,inst);
    lua_getfield(L,inst,"deps");
    const int deps = lua_gettop(L);
    if( lua_isnil(L,deps) )
    {
        lua_pop(L,1); // nil
        return;
    }
    size_t i;
    for( i = 1; i <= lua_objlen(L,deps); i++ )
    {
        lua_rawgeti(L,deps,i);
        const int dep = lua_gettop(L);

        mergeOut(L,inst,dep);

        lua_pop(L,1); // dep
    }

    lua_pop(L,1); // deps
}

static int calcDep(lua_State* L) // param: inst
{
    const int inst = 1;

    lua_getfield(L,inst,"#visited");
    const int done = !lua_isnil(L,-1);
    lua_pop(L,1);

    lua_pushboolean(L,1);
    lua_setfield(L,inst,"#visited");

#if 0
    lua_getfield(L,inst,"#decl");
    bs_declpath(L,-1,"."); // TEST
    fprintf(stdout,"decl%s: %s\n", (done?" done":""),lua_tostring(L,-1));
    fflush(stdout);
    lua_pop(L,2);
#endif

    if( done )
        return 0; // we're already calculated result set

    visitDeps(L,inst);

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getmetatable(L,inst);
    const int cls = lua_gettop(L);

    if( isa( L, builtins, cls, "Library" ) )
        libraryDep(L,inst,builtins,0);
    else if( isa( L, builtins, cls, "Executable") )
        ; // NOP
    else if( isa( L, builtins, cls, "SourceSet") )
        libraryDep(L,inst,builtins,1);
    else if( isa( L, builtins, cls, "Group") )
        groupDep(L,inst);
    else if( isa( L, builtins, cls, "Config") )
        ; // NOP
    else if( isa( L, builtins, cls, "LuaScript") )
        ; // NOP
    else if( isa( L, builtins, cls, "LuaScriptForeach") )
        ; // NOP
    else if( isa( L, builtins, cls, "Copy") )
        ; // NOP
    else if( isa( L, builtins, cls, "Message") )
        ; // NOP
    else if( isa( L, builtins, cls, "Moc") )
        ; // TODO
    else if( isa( L, builtins, cls, "Rcc") )
        ; // TODO
    else
    {
        lua_getfield(L,cls,"#name");
        luaL_error(L,"don't know how to process instances of class '%s'", lua_tostring(L,-1));
    }

    lua_pop(L,2); // builtins, cls
    return 0;
}

static void visitDeps(lua_State* L, int inst)
{
    lua_getfield(L,inst,"deps");
    const int deps = lua_gettop(L);

    if( lua_isnil(L,deps) )
    {
        lua_pop(L,1); // nil
        return;
    }

    size_t i;
    for( i = 1; i <= lua_objlen(L,deps); i++ )
    {
        // TODO: check for circular deps
        lua_pushcfunction(L, calcDep);
        lua_rawgeti(L,deps,i);
        lua_call(L,1,0);
    }
    lua_pop(L,1); // deps
}

static void iterateDeps(lua_State* L, int inst, int filter, int inverse, FILE* out,
                        void (*iterator)(lua_State* L, int inst, int item, FILE* out))
{
    const int top = lua_gettop(L);

    lua_getfield(L,inst,"deps");
    const int deps = lua_gettop(L);
    if( lua_isnil(L,deps) )
    {
        lua_pop(L,1); // nil
        return;
    }

    size_t i;
    if( inverse )
    {
        for( i = lua_objlen(L,deps); i > 0; i-- )
        {
            lua_rawgeti(L,deps,i);
            const int dep = lua_gettop(L);

            lua_getfield(L,dep,"#out");
            const int res = lua_gettop(L);

            if( !lua_isnil(L,res) )
            {
                size_t j;
                for( j = lua_objlen(L,res); j > 0; j-- )
                {
                    lua_rawgeti(L,res,j);
                    const int item = lua_gettop(L);
                    lua_getfield(L,item,"#kind");
                    const int k = lua_tointeger(L,-1);
                    lua_pop(L,1);
                    if( k == filter )
                        iterator(L,inst, item, out);
                    lua_pop(L,1); // item
                }
            }

            lua_pop(L,2); // dep, res
        }
    }else
    {
        for( i = 1; i <= lua_objlen(L,deps); i++ )
        {
            lua_rawgeti(L,deps,i);
            const int dep = lua_gettop(L);

            lua_getfield(L,dep,"#out");
            const int res = lua_gettop(L);

            if( !lua_isnil(L,res) )
            {
                size_t j;
                for( j = 1; j <= lua_objlen(L,res); j++ )
                {
                    lua_rawgeti(L,res,j);
                    const int item = lua_gettop(L);
                    lua_getfield(L,item,"#kind");
                    const int k = lua_tointeger(L,-1);
                    lua_pop(L,1);
                    if( k == filter )
                        iterator(L,inst, item, out);
                    lua_pop(L,1); // item
                }
            }

            lua_pop(L,2); // dep, res
        }
    }

    lua_pop(L,1); // deps

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void printSource(lua_State* L, int inst, int item, FILE* out)
{
    fwrite(s_listFill1,1,strlen(s_listFill1),out);
    lua_getfield(L,item,"#path");
    const char* path = lua_tostring(L,-1);
    fwrite(path,1,strlen(path),out);
    fwrite("\"",1,1,out);
    lua_pop(L,1); // path
}

static void addSources(lua_State* L, int inst, FILE* out)
{
    const int top = lua_gettop(L);
    const char* text = "SOURCES +="; // TODO: OBJECTIVE_SOURCES for .mm
    fwrite(text,1,strlen(text),out);

    iterateDeps(L,inst,BS_SourceFiles, 0,out,printSource);

    lua_getfield(L,inst,"sources");
    const int sources = lua_gettop(L);

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    size_t i;
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int file = lua_gettop(L);

        if( *lua_tostring(L,file) != '/' )
        {
            addPath(L,absDir,file);
            lua_replace(L,file);
        }

        const char* str = bs_denormalize_path(lua_tostring(L,file));
        fwrite(s_listFill1,1,strlen(s_listFill1),out);
        fwrite(str,1,strlen(str),out);
        fwrite("\"",1,1,out);

        lua_pop(L,1); // file
    }

    lua_pop(L,2); // sources, absDir

#if 0 // doesn't work
    const char* text2 = "\n\nHEADERS += $$files(*.h)\n";
    fwrite(text2,1,strlen(text2),out);
#endif

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void addSources2(lua_State* L, int inst, FILE* out)
{
    const int top = lua_gettop(L);

    lua_getfield(L,inst,"sources");
    const int sources = lua_gettop(L);

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    size_t i;
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int file = lua_gettop(L);

        if( *lua_tostring(L,file) != '/' )
        {
            addPath(L,absDir,file);
            lua_replace(L,file);
        }

        const char* str = bs_denormalize_path(lua_tostring(L,file));
        fwrite(s_listFill1,1,strlen(s_listFill1),out);
        fwrite(str,1,strlen(str),out);
        fwrite("\"",1,1,out);

        lua_pop(L,1); // file
    }

    lua_pop(L,2); // sources, absDir

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void addIncludes(lua_State* L, int inst, FILE* out, int head)
{
    const int top = lua_gettop(L);

    if( head )
    {
        const char* text = "INCLUDEPATH +=";
        fwrite(text,1,strlen(text),out);
    }

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        // TODO: check for circular deps
        addIncludes(L, config, out, 0 );
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    lua_getfield(L,inst,"include_dirs");
    const int includes = lua_gettop(L);

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    const char* fill = " \\\n\t\"";

    for( i = 1; i <= lua_objlen(L,includes); i++ )
    {
        lua_rawgeti(L,includes,i);
        const int include = lua_gettop(L);

        if( *lua_tostring(L,include) != '/' )
        {
            // relative path
            addPath(L,absDir,include);
            lua_replace(L,include);
        }
        const char* str = bs_denormalize_path(lua_tostring(L,include));

        fwrite(fill,1,strlen(fill),out);
        fwrite(str,1,strlen(str),out);
        fwrite("\"",1,1,out);

        lua_pop(L,1); // include
    }

    lua_pop(L,2); // includes, absDir

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void addDefines(lua_State* L, int inst, FILE* out, int head)
{
    const int top = lua_gettop(L);

    if( head )
    {
        const char* text = "DEFINES +=";
        fwrite(text,1,strlen(text),out);
    }

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        // TODO: check for circular deps
        addDefines(L, config, out, 0 );
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    lua_getfield(L,inst,"defines");
    const int defines = lua_gettop(L);

    const char* fill = " \\\n\t\"";

    for( i = 1; i <= lua_objlen(L,defines); i++ )
    {
        lua_rawgeti(L,defines,i);
        const int define = lua_gettop(L);

        fwrite(fill,1,strlen(fill),out);
        fwrite(lua_tostring(L,define),1,lua_objlen(L,define),out);
        fwrite("\"",1,1,out);

        lua_pop(L,1); // define
    }

    lua_pop(L,1); // defines

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void passOnLib(lua_State* L, int inst, int item, FILE* out)
{
    lua_getfield(L,inst,"#out");
    if( lua_isnil(L,-1) )
    {
        lua_pop(L,1);
        lua_createtable(L,0,0);
        lua_pushvalue(L,-1);
        lua_setfield(L,inst,"#out");
    }
    const int res = lua_gettop(L);
    assert( lua_istable(L,res) );
    lua_pushvalue(L,item);
    lua_rawseti(L,res, lua_objlen(L,res) + 1);
    lua_pop(L,1); // res
}

static void addLibs(lua_State* L, int inst, int kind, FILE* out, int head)
{
    const int top = lua_gettop(L);
    if( head )
    {
        const char* text = "LIBS +=";
        fwrite(text,1,strlen(text),out);
    }

    if( kind == BS_Executable || kind == BS_DynamicLib )
    {
        iterateDeps(L,inst,BS_DynamicLib,1,out,printSource);
        iterateDeps(L,inst,BS_StaticLib,1,out,printSource);
    }else if( kind == BS_StaticLib )
    {
        iterateDeps(L,inst,BS_DynamicLib,0,out,passOnLib);
        iterateDeps(L,inst,BS_StaticLib,0,out,passOnLib);
    }

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        // TODO: check for circular deps
        addLibs(L, config, kind, out, 0 );
        mergeOut(L,inst,config);
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    // TODO

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genLibrary(lua_State* L, int inst, int builtins, FILE* out, int forceStatic )
{
    const int top = lua_gettop(L);
    lua_getfield(L,inst,"lib_type");
    const int lib_type = forceStatic ? BS_StaticLib :
                                       strcmp(lua_tostring(L,-1),"shared") == 0 ? BS_DynamicLib : BS_StaticLib;
    lua_pop(L,1);

    const char* text = "QT -= core gui\n"
            "TEMPLATE = lib\n";
    fwrite(text,1,strlen(text),out);

    lua_getfield(L,inst,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_getfield(L,inst,"#decl");
        lua_getfield(L,-1,"#name");
        lua_replace(L,-2);
    }
    const char* text2 = "TARGET = ";
    fwrite(text2,1,strlen(text2),out);
    fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
    fwrite("\n",1,1,out);
    lua_pop(L,1); // name

    if( lib_type == BS_StaticLib )
    {
        const char* text = "CONFIG += staticlib\n";
        fwrite(text,1,strlen(text),out);
    }

    fwrite("\n",1,1,out);
    addDefines(L,inst,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addIncludes(L,inst,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addLibs(L,inst,lib_type,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addSources(L,inst,out);
    fwrite("\n\n",1,2,out);

    if( !forceStatic )
    {
        pushLibraryName(L,inst,builtins,forceStatic);
        const int path = lua_gettop(L);
        lua_pushfstring(L,"QMAKE_POST_LINK += $$QMAKE_COPY $$quote(%s) $$quote($$root_build_dir)\n\n", lua_tostring(L,path));
        fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
        lua_pop(L,2);
    }

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genExe(lua_State* L, int inst, int builtins, FILE* out )
{
    const int top = lua_gettop(L);
    const char* text = "QT -= core gui\n"
            "TEMPLATE = app\n\n";
    fwrite(text,1,strlen(text),out);

    lua_getfield(L,inst,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_getfield(L,inst,"#decl");
        lua_getfield(L,-1,"#name");
        lua_replace(L,-2);
    }
    const char* text2 = "TARGET = ";
    fwrite(text2,1,strlen(text2),out);
    fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
    fwrite("\n",1,1,out);
    lua_pop(L,1); // name

    fwrite("\n",1,1,out);
    addDefines(L,inst,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addLibs(L,inst,BS_Executable,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addIncludes(L,inst,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addSources(L,inst,out);
    fwrite("\n\n",1,2,out);

    pushExecutableName(L,inst,builtins);
    const int path = lua_gettop(L);
    lua_pushfstring(L,"QMAKE_POST_LINK += $$QMAKE_COPY $$quote(%s) $$quote($$root_build_dir)\n\n", lua_tostring(L,path));
    fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
    lua_pop(L,2);

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genAux(lua_State* L, int inst, FILE* out )
{

    const char* text = "QT -= core gui\n"
            "TEMPLATE = aux\n";
    fwrite(text,1,strlen(text),out);
}

static void genMoc(lua_State* L, int inst, FILE* out )
{
    const char* text = "QT -= core gui\n"
            "TEMPLATE = aux\n";
    fwrite(text,1,strlen(text),out);

    /*
    lua_getfield(L,inst,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_getfield(L,inst,"#decl");
        lua_getfield(L,-1,"#name");
        lua_replace(L,-2);
    }
    const int name = lua_gettop(L);
    const char* text2 = "TARGET = ";
    fwrite(text2,1,strlen(text2),out);
    fwrite(lua_tostring(L,name),1,lua_objlen(L,name),out);
    fwrite("\n",1,1,out);
    */

    fwrite("\n",1,1,out);
    addDefines(L,inst,out,1);
    fwrite("\n\n",1,2,out);

    const char* text7 = "MOC_SOURCES +=";
    fwrite(text7,1,strlen(text7),out);
    addSources2(L, inst, out);
    fwrite("\n\n",1,2,out);

    const char* text3 = "compiler.commands = $$lua_path "
            "\\\"$$root_project_dir/moc.lua\\\" "
        #if 0
            "\\\"$$root_build_dir/moc\\\" "
        #else
            "\\\"$$moc_path\\\" "
        #endif
            "\\\"${QMAKE_FILE_IN}\\\" "
            "\\\"$$shadowed($$PWD)\\\" "
            "$$join(DEFINES,\" \")"; // join is enough, since addDefines quotes if expression
    fwrite(text3,1,strlen(text3),out);
    fwrite("\n",1,1,out);

    const char* text5 = "compiler.input = MOC_SOURCES";
    fwrite(text5,1,strlen(text5),out);
    fwrite("\n",1,1,out);

    const char* text6 = "compiler.output = $$shadowed($$PWD)/moc_${QMAKE_FILE_BASE}.cpp";
    // NOTE: the exact name doesn't matter anyway since moc.lua checks if input is current itself
    // const char* text6 = "compiler.output_function = calc_moc_output";
    fwrite(text6,1,strlen(text6),out);
    fwrite("\n",1,1,out);

    const char* text4 = "QMAKE_EXTRA_COMPILERS += compiler";
    fwrite(text4,1,strlen(text4),out);
    fwrite(" \n",1,1,out);

    //lua_pop(L,1); // name
}

static int genmodule(lua_State* L) // arg: module def
{
    // NOTE: this approach, where we build depth first along the original module tree, doesn't work
    // If we e.g. try to build LeanQt with HAVE_OBJECT, the second level core is first built and within
    // it the run_moc and moc_sources projects, before tools on top-level is built.
    // So we have to do without the original module structure and instead linearize all modules depth-first
    // under a top-level subdirs project; this has the advantage that we get rid of the intermediate level
    // subdir projects.

    const int module = 1;
    const int top = lua_gettop(L);

    lua_getfield(L,module,"#inst");
    const int modinst = lua_gettop(L);

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    const int rootOutDir = lua_gettop(L);

    lua_getfield(L,module,"#rdir");
    const int relDir = lua_gettop(L);

    if( strcmp(lua_tostring(L,relDir),".") == 0 )
    {
        lua_pushvalue(L,rootOutDir);
        lua_pushstring(L,"/Project.pro");
        lua_concat(L,2);
    }else
    {
        addPath(L,rootOutDir,relDir);
        lua_pushstring(L,"/");
        lua_getfield(L,module,"#name");
        lua_pushstring(L,".pro");
        lua_concat(L,4);
    }
    const int path = lua_gettop(L);

    FILE* out = bs_fopen(bs_denormalize_path(lua_tostring(L,path)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,path));
    const char* text = "# generated by BUSY, do not modify\n"
            "QT -= core gui\n"
            "TEMPLATE = subdirs\n"
            "CONFIG += ordered\n"
            "SUBDIRS += \\\n";
    fwrite(text,1,strlen(text),out);

    size_t i;
    const size_t len = lua_objlen(L,module);
    for( i = 1; i <= len; i++ )
    {
        lua_rawgeti(L,module,i);
        const int decl = lua_gettop(L);
        lua_getfield(L,decl,"#qmake");
        if( !lua_isnil(L,-1) )
        {
            lua_getfield(L,module,"#qmake");
            lua_pushinteger(L,lua_tointeger(L,-1)+1);
            lua_replace(L,-2);
            lua_setfield(L,decl,"#qmake"); // assign the hierarchy level to each decl

            lua_getfield(L,decl,"#name");
            const int name = lua_gettop(L);
            addPath(L,rootOutDir,relDir);
            lua_pushstring(L,"/");
            lua_pushvalue(L,name);
            lua_concat(L,3);
            const int path = lua_gettop(L);

            if( !bs_exists(lua_tostring(L,path)) )
            {
                if( bs_mkdir(lua_tostring(L,path)) != 0 )
                    luaL_error(L,"error creating directory %s", lua_tostring(L,path));
                fflush(stdout);
            }
            fwrite("\t",1,1,out);
            fwrite(lua_tostring(L,name),1,lua_objlen(L,name),out);
            fwrite(" ",1,1,out);
            if( i < len )
                fwrite("\\",1,1,out);
            fwrite("\n",1,1,out);

            lua_getfield(L,decl,"#kind");
            const int k = lua_tointeger(L,-1);
            lua_pop(L,1);

            if( k == BS_ModuleDef )
            {
                lua_pushcfunction(L,genmodule);
                lua_pushvalue(L,decl);
                lua_call(L,1,0);
            }else if( k == BS_VarDecl )
            {
                lua_pushvalue(L,name);
                lua_rawget(L,modinst);
                const int productinst = lua_gettop(L);

                lua_pushvalue(L,path);
                lua_pushstring(L,"/");
                lua_pushvalue(L,name);
                lua_pushstring(L,".pro");
                lua_concat(L,4);

                FILE* out2 = bs_fopen(bs_denormalize_path(lua_tostring(L,-1)),"w");
                if( out2 == NULL )
                    luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,-1));
                const char* text = "# generated by BUSY, do not modify\n";

                fwrite(text,1,strlen(text),out2);
                lua_pop(L,1);

                visitDeps(L,productinst);

                lua_getmetatable(L,productinst);
                const int cls = lua_gettop(L);

                if( isa( L, builtins, cls, "Library" ) )
                    genLibrary(L,productinst, builtins, out2, 0);
                else if( isa( L, builtins, cls, "Executable") )
                    genExe(L,productinst,builtins,out2);
                else if( isa( L, builtins, cls, "SourceSet") )
                    genLibrary(L,productinst, builtins, out2, 1);
                else if( isa( L, builtins, cls, "Group") )
                    genAux(L,productinst, out2);
                else if( isa( L, builtins, cls, "Config") )
                    genAux(L,productinst, out2);
                else if( isa( L, builtins, cls, "LuaScript") )
                    genAux(L,productinst, out2); // TODO;
                else if( isa( L, builtins, cls, "LuaScriptForeach") )
                    genAux(L,productinst, out2); // TODO
                else if( isa( L, builtins, cls, "Copy") )
                    genAux(L,productinst, out2); // TODO
                else if( isa( L, builtins, cls, "Message") )
                    genAux(L,productinst, out2); // TODO
                else if( isa( L, builtins, cls, "Moc") )
                    genMoc(L,productinst, out2);
                else if( isa( L, builtins, cls, "Rcc") )
                    genAux(L,productinst, out2); // TODO;
                else
                {
                    fclose(out2);
                    lua_getfield(L,cls,"#name");
                    luaL_error(L,"don't know how to build instances of class '%s'", lua_tostring(L,-1));
                }

                fclose(out2);
                lua_pop(L,2); // productinst, cls
            }else
                assert(0);

            lua_pop(L,2); // name, path
        }

        lua_pop(L,2); // decl, qmake
    }

    fclose(out);
    lua_pop(L,6); // inst, builtins, binst, rootOutDir, relDir, path

    const int bottom = lua_gettop(L);
    assert(top == bottom);
    return 0;
}

int bs_genQmake(lua_State* L) // args: root module def, list of productinst
{
    enum { ROOT = 1, PRODS };
    const int top = lua_gettop(L);

    size_t i;
    for( i = 1; i <= lua_objlen(L,PRODS); i++ )
    {
        lua_pushcfunction(L,mark);
        lua_rawgeti(L,PRODS,i);
        lua_call(L,1,0);
    }

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    const int buildDir = lua_gettop(L);

    if( !bs_exists(lua_tostring(L,-1)) )
    {
        if( bs_mkdir(lua_tostring(L,-1)) != 0 )
            luaL_error(L,"error creating directory %s", lua_tostring(L,-1));
        fflush(stdout);
    }

    lua_pushvalue(L,buildDir);
    lua_pushstring(L,"/.qmake.conf");
    lua_concat(L,2);
    const int confPath = lua_gettop(L);

    lua_getfield(L,binst,"root_source_dir");
    const int sourceDir = lua_gettop(L);

    FILE* out = bs_fopen(bs_denormalize_path(lua_tostring(L,confPath)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,confPath));

    const char* text = "# generated by BUSY, do not modify\n"
                        "root_build_dir=$$shadowed($$PWD)\n"
                        "root_project_dir=$$PWD\n";
    fwrite(text,1,strlen(text),out);

#if 0
    // NOTE: $$basename(1) outputs "buffer.h" or "buffer.cpp", so it's not the base name!
    // NOTE: ${QMAKE_FILE_BASE} is not available in calc_moc_output
    const char* text3 = "defineReplace(calc_moc_output){ # args: $$1:QMAKE_FILE_IN\n"
                        "    contains($$1, ^.*\\.(cpp|cxx)$){\n"
                        "        return($$shadowed($$PWD)/$$basename(1).moc)\n"
                        "    }else{\n"
                        "        return($$shadowed($$PWD)/moc_$$basename(1).cpp)\n"
                        "    }\n"
                        "}\n";
    fwrite(text3,1,strlen(text3),out);
#endif

    const char* text3 = "include(config.pri)\n";
    fwrite(text3,1,strlen(text3),out);

    fclose(out);

    lua_pushvalue(L,buildDir);
    lua_pushstring(L,"/moc.lua");
    lua_concat(L,2);
    const int scriptPath = lua_gettop(L);

    out = bs_fopen(bs_denormalize_path(lua_tostring(L,scriptPath)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,scriptPath));

    const char* text4 = "-- generated by BUSY, do not modify\n"
            "B = require \"BUSY\"\n"
            "if #arg < 3 then error(\"moc.lua at least expects path-to-moc, in-file and out-dir "
                "as arguments, followed by 0..n defines\") end\n"
            "B.moc(unpack(arg,1))\n";
    fwrite(text4,1,strlen(text4),out);
    fclose(out);

    lua_pushvalue(L,buildDir);
    lua_pushstring(L,"/config.pri");
    lua_concat(L,2);
    const int configPath = lua_gettop(L);

    out = bs_fopen(bs_denormalize_path(lua_tostring(L,configPath)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,configPath));

    const char* text6 = "# generated by BUSY, do not modify\n"
            "# note that there is a possibly hidden .qmake.conf which includes this file\n"
            "root_source_dir = \"";
    fwrite(text6,1,strlen(text6),out);
    const int res = bs_makeRelative(lua_tostring(L,buildDir),lua_tostring(L,sourceDir));
    if( res == BS_OK )
    {
        fwrite(bs_global_buffer(),1,strlen(bs_global_buffer()),out);
        fwrite("\"\n",1,2,out);
    }else
    {
        const char* path = bs_denormalize_path(lua_tostring(L,sourceDir));
        fwrite(path,1,strlen(path),out); // RISK: this path is platform dependent
        fwrite("\"\n",1,2,out);
    }

    if( bs_thisapp() == BS_OK )
    {
        const char* text2 = "lua_path = \"";
        fwrite(text2,1,strlen(text2),out);
        const char* path = bs_denormalize_path(bs_global_buffer());
        fwrite(path,1,strlen(path),out);
        fwrite("\"\n",1,2,out);
    }

    const char* text5 = "moc_path = \"";
    fwrite(text5,1,strlen(text5),out);

    lua_getfield(L,binst,"moc_path");
    const int mocPath = lua_gettop(L);
    const char* defaultPath = "$$root_build_dir/moc";
    if( lua_isnil(L,mocPath) || strcmp(".",lua_tostring(L,mocPath)) == 0 )
    {
        lua_pushstring(L,defaultPath);
        lua_replace(L,mocPath);
    }else if( *lua_tostring(L,mocPath) != '/' )
        luaL_error(L,"moc_path cannot be relative: %s", lua_tostring(L,mocPath));
    else
    {
        const char* cmd = bs_denormalize_path(lua_tostring(L,mocPath));
        if( bs_exec(cmd) != 0 )
            cmd = defaultPath;
        lua_pushstring(L,cmd);
        lua_replace(L,mocPath);
    }
    fwrite(lua_tostring(L,mocPath),1,lua_objlen(L,mocPath),out);
    fwrite("\"\n",1,2,out);

    fclose(out);


    lua_pushcfunction(L,genmodule);
    lua_pushvalue(L,ROOT);
    lua_call(L,1,0);

    lua_pop(L,8); // builtins, binst, buildDir, path, sourceDir, scriptPath, mocPath
    assert( top == lua_gettop(L) );
    return 0;
}
