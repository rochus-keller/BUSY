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

//#define BS_QMAKE_GEN_ABS_SOURCE_PATHS
#define BS_QMAKE_HAVE_COPY

static int mark(lua_State* L) // args: productinst, order, no returns
{
    const int inst = 1;
    const int order = 2;

    const int top = lua_gettop(L);

    lua_getfield(L,inst,"#decl");
    const int decl = lua_gettop(L);

    lua_getfield(L,-1,"#qmake");
    if( lua_isnil(L,-1) )
    {
        bs_declpath(L,decl,".");
        lua_setfield(L,decl,"#qmake"); // mark decl
    }else
    {
        lua_pop(L,2); // decl, qmake
        return 0;
    }
    lua_pop(L,1); // qmake

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
        lua_pushvalue(L,order);
        lua_call(L,2,0);
    }
    lua_pop(L,1); // deps

    lua_pushvalue(L,decl);
    lua_rawseti(L,order,lua_objlen(L,order)+1 );

    lua_pop(L,1); // decl

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

static void pushExecutableName(lua_State* L, int inst, int builtins, int nameOnly)
{
    const int top = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"target_os");
    const int win32 = strcmp(lua_tostring(L,-1),"win32") == 0 || strcmp(lua_tostring(L,-1),"winrt") == 0;
    lua_pop(L,2); // target_os, binst

    lua_getfield(L,inst,"#decl");
    const int decl = lua_gettop(L);

    lua_getfield(L,decl,"#qmake");
    const int declpath = lua_gettop(L);

    lua_getfield(L,decl,"#name");
    const int declname = lua_gettop(L);

    if( nameOnly )
        lua_pushstring(L,"");
    else
        lua_pushfstring(L,"$$root_build_dir/%s/", lua_tostring(L,declpath));
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

    lua_concat(L,3);

    lua_replace(L,decl);

    lua_pop(L,2); // declpath, declname

    assert( top + 1 == lua_gettop(L) );
}

static int pushLibraryPath(lua_State* L, int inst, int builtins, int isSourceSet, int nameOnly, int forLinking)
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
    const int lib_type = isSourceSet || str == 0 ? BS_StaticLib :
                                       strcmp(str,"shared") == 0 ? BS_DynamicLib : BS_StaticLib;
    lua_pop(L,1);

    lua_getfield(L,inst,"#decl");
    const int decl = lua_gettop(L);

    lua_getfield(L,decl,"#qmake");
    const int declpath = lua_gettop(L);

    lua_getfield(L,decl,"#name");
    const int declname = lua_gettop(L);

    if( nameOnly )
        lua_pushstring(L,"");
    else
        lua_pushfstring(L,"$$root_build_dir/%s/", lua_tostring(L,declpath));
    if( win32 )
        lua_pushstring(L,"");
    else
        lua_pushstring(L,"lib"); // prefix
    lua_getfield(L,inst,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_pushvalue(L,declname);
    }

    if( lib_type == BS_DynamicLib )
    {
        if( win32 )
        {
            if( forLinking )
                lua_pushstring(L,".lib");
            else
                lua_pushstring(L,".dll");
        }else if(mac)
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
    lua_concat(L,4);

    lua_replace(L,decl);

    lua_pop(L,2); // declpath, declname

    const int bottom = lua_gettop(L);
    assert( top + 1 == bottom );
    return lib_type;
}

static void pushObjectFileName(lua_State* L, int declpath, int source, int toolchain)
{
    const int top = lua_gettop(L);

    int len;
    const char* name = bs_path_part(lua_tostring(L,source),BS_baseName,&len);
    lua_pushlstring(L,name,len);
    lua_pushfstring(L,"$$root_build_dir/%s/%s",lua_tostring(L,declpath), lua_tostring(L,-1));
    lua_replace(L,-2);

    if( toolchain == BS_msvc )
        lua_pushstring(L,".obj");
    else
        lua_pushstring(L,".o");

    lua_concat(L,2);

    const int bottom = lua_gettop(L);
    assert( top + 1 == bottom );
}

static void libraryDep(lua_State* L, int inst, int builtins, int isSourceSet)
{
    const int top = lua_gettop(L);
    visitDeps(L,inst);

    const int lib_type = pushLibraryPath(L,inst,builtins,isSourceSet,0,1);
    const int path = lua_gettop(L);

    lua_getfield(L,inst,"#out");
    const int out = lua_gettop(L);

    if( isSourceSet )
    {
        addDep(L,out,BS_SourceSetLib,path);

        lua_getfield(L,inst,"#decl");
        lua_getfield(L,-1,"#qmake");
        lua_replace(L,-2);
        const int declpath = lua_gettop(L);

        lua_getfield(L,builtins,"#inst");
        const int binst = lua_gettop(L);
        const int toolchain = bs_getToolchain(L,binst);
        lua_pop(L,1); // binst

        lua_getfield(L,inst,"sources");
        const int sources = lua_gettop(L);
        size_t i;
        for( i = 1; i <= lua_objlen(L,sources); i++ )
        {
            lua_rawgeti(L,sources,i);
            const int source = lua_gettop(L);
            pushObjectFileName(L,declpath,source,toolchain);
            const int name = lua_gettop(L);
            addDep(L,out,BS_ObjectFiles,name);
            lua_pop(L,2); // source, name
        }
        lua_pop(L,1); // sources

        // for the BS_ObjectFiles you pass also consider BS_SourceFiles from deps
        lua_getfield(L,inst,"deps");
        const int deps = lua_gettop(L);
        if( !lua_isnil(L,deps) )
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
                    for( j = lua_objlen(L,res); j > 0; j-- )
                    {
                        lua_rawgeti(L,res,j);
                        const int item = lua_gettop(L);
                        lua_getfield(L,item,"#kind");
                        const int k = lua_tointeger(L,-1);
                        lua_pop(L,1); // kind
                        if( k == BS_SourceFiles )
                        {
                            lua_getfield(L,item,"#path");
                            const int path = lua_gettop(L);
                            pushObjectFileName(L,declpath,path,toolchain);
                            const int name = lua_gettop(L);
                            addDep(L,out,BS_ObjectFiles,name);
                            lua_pop(L,2); // path, name
                        }
                        lua_pop(L,1); // item
                    }
                }
                lua_pop(L,2); // dep, res
            }
        }
        lua_pop(L,2); // deps, declpath
    }else
        addDep(L,out,lib_type,path);

    lua_pop(L,2); // path, out

    assert( top == lua_gettop(L) );
}

static void exeDep(lua_State* L, int inst, int builtins)
{
    const int top = lua_gettop(L);
    visitDeps(L,inst);

    lua_getfield(L,inst,"#out");
    const int out = lua_gettop(L);

    pushExecutableName(L,inst,builtins,0);
    const int path = lua_gettop(L);

    addDep(L,out,BS_Executable,path);

    lua_pop(L,2); // out, path

    assert( top == lua_gettop(L) );
}

static void scriptDep(lua_State* L, int inst, int builtins)
{
    const int top = lua_gettop(L);
    visitDeps(L,inst);

    lua_getfield(L,inst,"#out");
    const int out = lua_gettop(L);

    lua_getfield(L,inst,"#decl");
    lua_getfield(L,-1,"#qmake");
    lua_replace(L,-2);
    const int declpath = lua_gettop(L);

    lua_getfield(L,inst,"outputs");
    const int outputs = lua_gettop(L);
    size_t j;
    for( j = 1; j <= lua_objlen(L,outputs); j++ )
    {
        lua_rawgeti(L,outputs,j);
        const int src = lua_gettop(L);

        if( *lua_tostring(L,src) != '/' )
        {
            lua_pushfstring(L,"$$root_build_dir/%s/%s",lua_tostring(L,declpath), lua_tostring(L,src));
            lua_replace(L,src);
        }else
            luaL_error(L,"the 'outputs' field requires relative paths");

        addDep(L,out,BS_SourceFiles,src);
        lua_pop(L,1); // src
    }
    lua_pop(L,3); // out, outDir, declpath
    assert( top == lua_gettop(L) );
}

static void mocDep(lua_State* L, int inst)
{
    const int top = lua_gettop(L);
    visitDeps(L,inst);

    lua_getfield(L,inst,"#out");
    const int out = lua_gettop(L);

    lua_getfield(L,inst,"#decl");
    lua_getfield(L,-1,"#qmake");
    lua_replace(L,-2);
    const int declpath = lua_gettop(L);

    lua_getfield(L,inst,"sources");
    const int sources = lua_gettop(L);

    size_t i;
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int source = lua_gettop(L);

        const int lang = bs_guessLang(lua_tostring(L,source));

        int len;
        const char* name = bs_path_part(lua_tostring(L,source),BS_baseName,&len);
        lua_pushlstring(L,name,len);
        if( lang == BS_header )
            // this file is automatically passed to the compiler over the deps chain; the user doesn't see it
            lua_pushfstring(L,"$$root_build_dir/%s/moc_%s.cpp",lua_tostring(L,declpath), lua_tostring(L,-1));
        else
            // this file has to be included at the bottom of the cpp file, so use the naming of the Qt documentation.
            lua_pushfstring(L,"$$root_build_dir/%s/%s.moc",lua_tostring(L,declpath), lua_tostring(L,-1));
        lua_replace(L,-2);
        const int outFile = lua_gettop(L);

        if( lang == BS_header )
            addDep(L,out,BS_SourceFiles,outFile);
#if 0
        // no longer used because we remap the existing build_dir() paths instead to $$root_build_dir/declpath
        else
        {
            // for x.moc we also pass on the include dir which is then propagated all the way up.
            // this is a work-around since the build_dir() path points to the wrong place in qmake gen
            lua_pushfstring(L,"$$root_build_dir/%s",lua_tostring(L,declpath));
            const int path = lua_gettop(L);
            addDep(L,out,BS_IncludeFiles,path);
            lua_pop(L,1);
        }
#endif

        lua_pop(L,2); // source, outFile
    }

    lua_pop(L,3); // out, declpath, sources

    assert( top == lua_gettop(L) );
}

static void rccDep(lua_State* L, int inst)
{
    const int top = lua_gettop(L);
    visitDeps(L,inst);

    lua_getfield(L,inst,"#out");
    const int out = lua_gettop(L);

    lua_getfield(L,inst,"#decl");
    lua_getfield(L,-1,"#qmake");
    lua_replace(L,-2);
    const int declpath = lua_gettop(L);

    lua_getfield(L,inst,"sources");
    const int sources = lua_gettop(L);

    size_t i;
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int source = lua_gettop(L);

        int len;
        const char* name = bs_path_part(lua_tostring(L,source),BS_baseName,&len);
        lua_pushlstring(L,name,len);
        // this file is automatically passed to the compiler over the deps chain; the user doesn't see it
        lua_pushfstring(L,"$$root_build_dir/%s/qrc_%s.cpp",lua_tostring(L,declpath), lua_tostring(L,-1));
        lua_replace(L,-2);
        const int outFile = lua_gettop(L);

        addDep(L,out,BS_SourceFiles,outFile);

        lua_pop(L,2); // source, outFile
    }

    lua_pop(L,3); // out, declpath, sources

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

typedef enum BS_Class { BS_NoClass, BS_LibraryClass, BS_ExecutableClass, BS_SourceSetClass,
                        BS_GroupClass, BS_ConfigClass,
                        BS_LuaScriptClass, BS_LuaScriptForEachClass, BS_CopyClass, BS_MessageClass,
                        BS_MocClass, BS_RccClass } BS_Class;
static const char* getClassName(int cls)
{
    switch( cls )
    {
    case BS_LibraryClass:
        return "Library";
    case BS_ExecutableClass:
        return "Executable";
    case BS_SourceSetClass:
        return "SourceSet";
    case BS_GroupClass:
        return "Group";
    case BS_ConfigClass:
        return "Config";
    case BS_LuaScriptClass:
        return "LuaScript";
    case BS_LuaScriptForEachClass:
        return "LuaScriptForEach";
    case BS_CopyClass:
        return "Copy";
    case BS_MessageClass:
        return "Message";
    case BS_MocClass:
        return "Moc";
    case BS_RccClass:
        return "Rcc";
    default:
        return "<unknown>";
    }
}

static int getClass(lua_State* L, int prodinst, int builtins)
{
    lua_getmetatable(L,prodinst);
    const int cls = lua_gettop(L);
    int res = BS_NoClass;
    if( isa( L, builtins, cls, "Library" ) )
        res = BS_LibraryClass;
    else if( isa( L, builtins, cls, "Executable") )
        res = BS_ExecutableClass;
    else if( isa( L, builtins, cls, "SourceSet") )
        res = BS_SourceSetClass;
    else if( isa( L, builtins, cls, "Group") )
        res = BS_GroupClass;
    else if( isa( L, builtins, cls, "Config") )
        res = BS_ConfigClass;
    else if( isa( L, builtins, cls, "LuaScript") )
        res = BS_LuaScriptClass;
    else if( isa( L, builtins, cls, "LuaScriptForeach") )
        res = BS_LuaScriptForEachClass;
    else if( isa( L, builtins, cls, "Copy") )
        res = BS_CopyClass;
    else if( isa( L, builtins, cls, "Message") )
        res = BS_MessageClass;
    else if( isa( L, builtins, cls, "Moc") )
        res = BS_MocClass;
    else if( isa( L, builtins, cls, "Rcc") )
        res = BS_RccClass;
    lua_pop(L,1); // cls
    return res;
}

static void assureOut(lua_State* L, int inst)
{
    lua_getfield(L,inst,"#out");
    if( lua_isnil(L,-1) )
    {
        lua_pop(L,1);
        lua_createtable(L,0,0);
        lua_setfield(L,inst,"#out");
    }else
        lua_pop(L,1);
}

static int calcDep(lua_State* L) // param: inst
{
    const int inst = 1;

    lua_getfield(L,inst,"#visited");
    const int done = !lua_isnil(L,-1);
    lua_pop(L,1);

    lua_pushboolean(L,1);
    lua_setfield(L,inst,"#visited");

    if( done )
        return 0; // we're already calculated result set

    visitDeps(L,inst);

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getmetatable(L,inst);
    const int cls = lua_gettop(L);

    switch( getClass(L,inst,builtins) )
    {
    case BS_LibraryClass:
        assureOut(L,inst);
        libraryDep(L,inst,builtins,0);
        break;
    case BS_ExecutableClass:
        assureOut(L,inst);
        exeDep(L,inst,builtins);
        break;
    case BS_ConfigClass:
    case BS_LuaScriptForEachClass:
    case BS_CopyClass:
    case BS_MessageClass:
        break; // NOP
    case BS_LuaScriptClass:
        assureOut(L,inst);
        scriptDep(L,inst,builtins);
        break;
    case BS_SourceSetClass:
        assureOut(L,inst);
        libraryDep(L,inst,builtins,1);
        break;
    case BS_GroupClass:
        groupDep(L,inst);
        break;
    case BS_MocClass:
        assureOut(L,inst);
        mocDep(L,inst);
        break;
    case BS_RccClass:
        assureOut(L,inst);
        rccDep(L,inst);
        break;
    default:
        lua_getfield(L,cls,"#name");
        luaL_error(L,"don't know how to process instances of class '%s'", lua_tostring(L,-1));
        break;
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

static int s_sourceCount = 0;

static void renderDep(lua_State* L, int inst, int item, FILE* out)
{
    fwrite(s_listFill1,1,strlen(s_listFill1),out);
    lua_getfield(L,item,"#path");
    const char* path = lua_tostring(L,-1);
    fwrite(path,1,strlen(path),out);
    fwrite("\"",1,1,out);
    lua_pop(L,1); // path
    s_sourceCount++;
}

static int addSources(lua_State* L, int inst, FILE* out, int withHeaderDeps)
{
    const int top = lua_gettop(L);

    int n = 0;
    if( withHeaderDeps )
    {
        const char* text = "SOURCES +="; // NOTE: apparently separate OBJECTIVE_SOURCES for *.mm not necessary
        fwrite(text,1,strlen(text),out);

        s_sourceCount = 0;
        iterateDeps(L,inst,BS_SourceFiles, 0,out,renderDep);
        n = s_sourceCount; // RISK this works as long we don't use threads
    }

    lua_getfield(L,inst,"sources");
    const int sources = lua_gettop(L);

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    bs_getModuleVar(L,inst,"#fsrdir");
    const int relDir = lua_gettop(L);

    size_t i;
    const size_t len = lua_objlen(L,sources);
    n += len;
    for( i = 1; i <= len; i++ )
    {
        lua_rawgeti(L,sources,i);
        const int file = lua_gettop(L);

        if( *lua_tostring(L,file) != '/' )
        {
#ifndef BS_QMAKE_GEN_ABS_SOURCE_PATHS
            lua_pushstring(L,"../"); // we're always in a subdir of root_project_dir
            lua_pushstring(L,"$$root_source_dir/");
            addPath(L,relDir,file);
            lua_pushstring(L,lua_tostring(L,-1));
            lua_replace(L,-2);
            lua_concat(L,3);
#else
            addPath(L,absDir,file);
#endif
            lua_replace(L,file);
        }

        const char* str = bs_denormalize_path(lua_tostring(L,file));
        fwrite(s_listFill1,1,strlen(s_listFill1),out);
        fwrite(str,1,strlen(str),out);
        fwrite("\"",1,1,out);

        lua_pop(L,1); // file
    }

    lua_pop(L,3); // sources, absDir, relDir


    const int bottom = lua_gettop(L);
    assert( top ==  bottom);

    return n;
}

static void addHeaders(lua_State* L, int inst, FILE* out)
{
    const int top = lua_gettop(L);

#ifndef BS_QMAKE_GEN_ABS_SOURCE_PATHS
    bs_getModuleVar(L,inst,"#fsrdir");
    const int relDir = lua_gettop(L);

    lua_pushfstring(L, "HEADERS += $$files(../$$root_source_dir/%s/*.h)", lua_tostring(L,relDir) );
#else
    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    lua_pushfstring(L, "HEADERS += $$files(%s/*.h)", bs_denormalize_path(lua_tostring(L,absDir)) );
#endif
    fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);

    lua_pop(L,2); // relDir, fstring

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void passOnDep(lua_State* L, int inst, int item, FILE* out)
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

static void renderInclude(lua_State* L, int inst, int item, FILE* out)
{
    renderDep(L,inst,item,out);
    // this is a work-around for x.moc files; we just pass dem on the dependency chain, since we don't know who is
    // depending on them
    passOnDep(L,inst,item,out);
}

static int findModulePath(lua_State* L, int inst, int builtins, const char* path)
{
    const int top = lua_gettop(L);

    lua_getfield(L,inst,"#decl");
    const int decl = lua_gettop(L);
    lua_getfield(L,decl,"#owner");
    const int module = lua_gettop(L);
    while(1)
    {
        lua_getfield(L,module,"#owner");
        if( lua_isnil(L,-1) )
        {
            lua_pop(L,1);
            break;
        }
        lua_replace(L,module);
    }
    // module is root now

    const char* slash = path;
    // path always points to a module, not a decl
    while( *slash )
    {
        while( *slash && *slash != '/' )
            slash++;
        lua_pushlstring(L, path, slash - path);
        path = slash + 1;
        if( *slash )
            slash++;
        lua_rawget(L,module);
        lua_replace(L,module);
        if( lua_isnil(L,module) )
            break;
    }
    int n = 0;
    if( !lua_isnil(L,module) )
    {
        // now look for Moc class decls in module
        size_t i;
        for( i = 1; i <= lua_objlen(L,module); i++ )
        {
            lua_rawgeti(L,module,i);
            const int decl = lua_gettop(L);
            lua_getfield(L,decl,"#qmake");
            int isMoc = 0;
            if( !lua_isnil(L,-1) )
            {
                lua_getfield(L,decl,"#type");
                const int cls = lua_gettop(L);
                isMoc = isa( L, builtins, cls, "Moc")
                        || isa( L, builtins, cls, "LuaScript")
                        || isa( L, builtins, cls, "LuaScriptForeach");
                lua_pop(L,1); // cls
                if(!isMoc)
                    lua_pop(L,1); // qmake
            }else
                lua_pop(L,1); // nil qmake
            if( isMoc )
            {
                n++;
                lua_replace(L,decl);
            }else
                lua_pop(L,1); // decl
        }
    }

    lua_remove(L,decl); // decl
    lua_remove(L,decl); // module

    const int bottom = lua_gettop(L);
    assert( top+n ==  bottom);

    return n;
}

static void renderQuotedPath(lua_State* L, int include, FILE* out)
{
    const char* str = bs_denormalize_path(lua_tostring(L,include));
    fwrite(s_listFill1,1,strlen(s_listFill1),out);
    fwrite(str,1,strlen(str),out);
    fwrite("\"",1,1,out);
}

static int remapPath(lua_State* L, int builtins, int path)
{
    lua_getfield(L,builtins,"#inst");
    lua_getfield(L,-1,"root_build_dir");
    lua_replace(L,-2);
    const int rootBuildDir = lua_gettop(L);
    const size_t rbdlen = strlen(lua_tostring(L,rootBuildDir));

    int res = 0;
    if( strncmp(lua_tostring(L,rootBuildDir),lua_tostring(L,path),rbdlen) == 0 )
    {
        // we have a path pointing to build_dir(); remap it to $$root_build_dir
        const char* residue = lua_tostring(L,path) + rbdlen;
        lua_pushfstring(L,"$$root_build_dir%s", residue);
        lua_replace(L,path);
        res = 1;
    }
    lua_pop(L,1); // rootBuildDir
    return res;
}

static void addIncludes(lua_State* L, int inst, int builtins, FILE* out, int head)
{
    const int top = lua_gettop(L);

    if( head )
    {
        const char* text = "INCLUDEPATH +=";
        fwrite(text,1,strlen(text),out);
    }

    // NOTE: does only work if sources directly depends on run_moc; if there is a common root depending on
    // moc_sources and sources in parallel, it doesnt work
    // iterateDeps(L,inst,BS_IncludeFiles, 0,out,renderInclude);

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        addIncludes(L, config, builtins, out, 0 );
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    lua_getfield(L,inst,"include_dirs");
    const int includes = lua_gettop(L);

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    bs_getModuleVar(L,inst,"#fsrdir");
    const int relDir = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    lua_getfield(L,-1,"root_build_dir");
    lua_replace(L,-2);
    const int rootBuildDir = lua_gettop(L);
    const size_t rbdlen = strlen(lua_tostring(L,rootBuildDir));

    for( i = 1; i <= lua_objlen(L,includes); i++ )
    {
        lua_rawgeti(L,includes,i);
        const int include = lua_gettop(L);

        if( *lua_tostring(L,include) != '/' )
        {
            // relative path
#ifndef BS_QMAKE_GEN_ABS_SOURCE_PATHS
            lua_pushstring(L,"../"); // we're always in a subdir of root_project_dir
            lua_pushstring(L,"$$root_source_dir/");
            addPath(L,relDir,include);
            lua_pushstring(L,lua_tostring(L,-1));
            lua_replace(L,-2);
            lua_concat(L,3);
#else
            addPath(L,absDir,include);
#endif
            lua_replace(L,include);
        }
        if( strncmp(lua_tostring(L,rootBuildDir),lua_tostring(L,include),rbdlen) == 0 )
        {
            // we have an include pointing to build_dir(); remap it to $$root_build_dir/qmake
            const char* path = lua_tostring(L,include) + rbdlen + 1; // skip first '/'
            const int n = findModulePath(L,inst,builtins,path);
            size_t j;
            for(j=n; j>0; j--)
            {
                lua_pushfstring(L,"$$root_build_dir/%s", lua_tostring(L, lua_gettop(L) - j + 1));
                renderQuotedPath(L,-1,out);
                lua_pop(L,1);
            }
            lua_pop(L,n);
        }else
            renderQuotedPath(L,include,out);

        lua_pop(L,1); // include
    }

    lua_pop(L,4); // includes, absDir, relDir, rootBuildDir

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
        addDefines(L, config, out, 0 );
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    lua_getfield(L,inst,"defines");
    const int defines = lua_gettop(L);

    lua_getglobal(L, "require");
    lua_pushstring(L, "string");
    lua_call(L,1,1);
    const int strlib = lua_gettop(L);

    for( i = 1; i <= lua_objlen(L,defines); i++ )
    {
        lua_rawgeti(L,defines,i);
        const int define = lua_gettop(L);

        fwrite(s_listFill1,1,strlen(s_listFill1),out);

        // given: "DEFAULT_XKB_RULES=\"evdev\""
        // qmake requires "DEFAULT_XKB_RULES=\\\"evdev\\\""
        if( strstr(lua_tostring(L,define),"\\\"") != NULL )
        {
            lua_getfield(L,strlib,"gsub");
            lua_pushvalue(L,define);
            lua_pushstring(L,"\\\"");
            lua_pushstring(L,"\\\\\\\"");
            lua_call(L,3,1);
            lua_replace(L,define);
        }
        fwrite(lua_tostring(L,define),1,lua_objlen(L,define),out);
        fwrite("\"",1,1,out);

        lua_pop(L,1); // define
    }

    lua_pop(L,2); // defines, strlib

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void addFlags(lua_State* L, int inst, FILE* out, int head, const char* header, const char* field)
{
    const int top = lua_gettop(L);

    if( head )
    {
        fwrite(header,1,strlen(header),out);
        const char* text = " +=";
        fwrite(text,1,strlen(text),out);
    }

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        addFlags(L, config, out, 0, header, field );
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    lua_getfield(L,inst,field);
    const int flags = lua_gettop(L);

    for( i = 1; i <= lua_objlen(L,flags); i++ )
    {
        lua_rawgeti(L,flags,i);
        const int flag = lua_gettop(L);

        fwrite(s_listFill1,1,strlen(s_listFill1),out);
        fwrite(lua_tostring(L,flag),1,lua_objlen(L,flag),out);
        fwrite("\"",1,1,out);

        lua_pop(L,1); // flag
    }

    lua_pop(L,1); // flags

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void addDepLibs(lua_State* L, int inst, int builtins, int kind, FILE* out)
{
    const int top = lua_gettop(L);

    const char* text = "LIBS +=";
    fwrite(text,1,strlen(text),out);

    lua_getfield(L,builtins,"#inst");
    lua_getfield(L,-1,"target_os");
    const int isLinux = strcmp(lua_tostring(L,-1),"linux") == 0;
    lua_pop(L,2); // binst, target_os

    if( kind == BS_DynamicLib )
    {
        iterateDeps(L,inst,BS_ObjectFiles,1,out,renderDep);
    }

    if( ( kind == BS_DynamicLib || kind == BS_Executable ) && isLinux )
    {
        // NOTE: image_sources and gui_sources depend on each other; since SourceSets are static libs here
        // the linker complains. The order cannot be fixed since both orders have missing dependencies.
        // work-around is --start-group/end-group
        // NOTE: start-group/end-group must not mix cpp and c libraries, otherwise symbols cannot be found;
        // I had this issue when first -lxcb was included in the group
        fwrite(s_listFill1,1,strlen(s_listFill1),out);
        const char* text3 = "-Wl,--start-group\"";
        fwrite(text3,1,strlen(text3),out);
    }

    if( kind == BS_DynamicLib )
    {
        iterateDeps(L,inst,BS_DynamicLib,1,out,renderDep);
        iterateDeps(L,inst,BS_StaticLib,1,out,renderDep);
    }else if( kind == BS_Executable )
    {
        iterateDeps(L,inst,BS_DynamicLib,1,out,renderDep);
        iterateDeps(L,inst,BS_StaticLib,1,out,renderDep);
        iterateDeps(L,inst,BS_SourceSetLib,0,out,renderDep);
    }

    if( ( kind == BS_DynamicLib || kind == BS_Executable ) && isLinux )
    {
        fwrite(s_listFill1,1,strlen(s_listFill1),out);
        const char* text3 = "-Wl,--end-group\"";
        fwrite(text3,1,strlen(text3),out);
    }

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

enum { BS_ForwardSourceSet, BS_ForwardStatic, BS_ForwardShared };
static void forwardDepLibs(lua_State* L, int inst, int kind, FILE* out)
{
    const int top = lua_gettop(L);

    switch(kind)
    {
    case BS_ForwardShared:
        // don't forward
        // a dynamic lib (like an executable) is the endpoint of forwarding
        break;
    case BS_ForwardStatic:
        // a true static library.

        // dependend static libs are not merged with this static lib, but forwarded to the client
        // to be used in parallel with this static lib
        iterateDeps(L,inst,BS_StaticLib,0,out,passOnDep);

        // this static lib cannot make use of dynamic libs and just forwards it
        iterateDeps(L,inst,BS_DynamicLib,0,out,passOnDep);

        // same reasoning as with BS_StaticLib
        iterateDeps(L,inst,BS_SourceSetLib,0,out,passOnDep);

        // we don't forward object files, since these could be added to this static lib, but we already
        // have the static lib of the source set which we forward, so we don't have to also send the object files
        break;
    case BS_ForwardSourceSet:
        // a source set just translates sources to object files and just passes through everything from its dependencies
        iterateDeps(L,inst,BS_StaticLib,0,out,passOnDep);
        iterateDeps(L,inst,BS_DynamicLib,0,out,passOnDep);
        iterateDeps(L,inst,BS_SourceSetLib,0,out,passOnDep);
        iterateDeps(L,inst,BS_ObjectFiles,0,out,passOnDep);
        break;
    }

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void addLibs(lua_State* L, int inst, int kind, FILE* out, int head, int ismsvc)
{
    const int top = lua_gettop(L);
    if( head )
    {
        const char* text = "LIBS +=";
        fwrite(text,1,strlen(text),out);
    }

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        addLibs(L, config, kind, out, 0, ismsvc );
        mergeOut(L,inst,config);
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    if( kind != BS_StaticLib )
    {
        bs_getModuleVar(L,inst,"#dir");
        const int absDir = lua_gettop(L);

        bs_getModuleVar(L,inst,"#fsrdir");
        const int relDir = lua_gettop(L);

        lua_getfield(L,inst,"lib_dirs");
        const int ldirs = lua_gettop(L);

        for( i = 1; i <= lua_objlen(L,ldirs); i++ )
        {
            lua_rawgeti(L,ldirs,i);
            const int path = lua_gettop(L);
            if( *lua_tostring(L,-1) != '/' )
            {
                // relative path
#ifndef BS_QMAKE_GEN_ABS_SOURCE_PATHS
                lua_pushstring(L,"../"); // we're always in a subdir of root_project_dir
                lua_pushstring(L,"$$root_source_dir/");
                addPath(L,relDir,path);
                lua_pushstring(L,lua_tostring(L,-1));
                lua_replace(L,-2);
                lua_concat(L,3);
#else
                addPath(L,absDir,path);
#endif
                lua_replace(L,path);
            }
            if(ismsvc)
                lua_pushfstring(L,"/libpath:%s", bs_denormalize_path(lua_tostring(L,path)) );
            else
                lua_pushfstring(L,"-L%s", bs_denormalize_path(lua_tostring(L,path)) );
            fwrite(s_listFill1,1,strlen(s_listFill1),out);
            fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
            fwrite("\"",1,1,out);
            lua_pop(L,2); // path, string
        }

        lua_getfield(L,inst,"lib_names");
        const int lnames = lua_gettop(L);
        for( i = 1; i <= lua_objlen(L,lnames); i++ )
        {
            lua_rawgeti(L,lnames,i);
            if(ismsvc)
                lua_pushfstring(L,"%s.lib", lua_tostring(L,-1));
            else
                lua_pushfstring(L,"-l%s", lua_tostring(L,-1));
            fwrite(s_listFill1,1,strlen(s_listFill1),out);
            fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
            fwrite("\"",1,1,out);
            lua_pop(L,2); // name, string
        }
        lua_getfield(L,inst,"frameworks");
        const int fworks = lua_gettop(L);
        for( i = 1; i <= lua_objlen(L,fworks); i++ )
        {
            fwrite(s_listFill1,1,strlen(s_listFill1),out);
            lua_rawgeti(L,fworks,i);
            lua_pushfstring(L,"-framework %s\"", lua_tostring(L,-1));
            fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
            lua_pop(L,2); // name, string
        }
        lua_pop(L,5); // ldirs, absDir, relDir, lnames, fworks
        // TODO: def_file, lib_files
    }


    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genCommon(lua_State* L, int inst, int builtins, int kind, FILE* out )
{
    // NOTE we don't consider #ctdefaults (i.e. set_defaults) here, since setting
    // these low-level, generic stuff is the business of qmake

    fwrite("\n",1,1,out);
    addDefines(L,inst,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addIncludes(L,inst, builtins,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addHeaders(L,inst,out);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    const int nSources = addSources(L,inst,out,1);
    if( nSources == 0 )
    {
        fwrite(s_listFill1,1,strlen(s_listFill1),out);
#ifndef BS_QMAKE_GEN_ABS_SOURCE_PATHS
        lua_pushstring(L,"$$root_project_dir/dummy.c\"");
        fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
        lua_pop(L,1);
#else
        lua_getfield(L,builtins,"#inst");
        lua_getfield(L,-1,"root_build_dir");
        lua_pushfstring(L,"%s/dummy.c\"",lua_tostring(L,-1));
        const char* path = bs_denormalize_path(lua_tostring(L,-1));
        fwrite(path,1,strlen(path),out);
        lua_pop(L,3);
#endif
    }
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addFlags(L,inst,out,1, "QMAKE_CXXFLAGS", "cflags_cc" );
    addFlags(L,inst,out,0, "", "cflags" );
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addFlags(L,inst,out,1, "QMAKE_CFLAGS", "cflags_c" );
    addFlags(L,inst,out,0, "", "cflags" );
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addFlags(L,inst,out,1, "QMAKE_LFLAGS", "ldflags" );
    fwrite("\n\n",1,2,out);

    // TODO cflags_objc, cflags_objcc
}

static void genLibrary(lua_State* L, int inst, int builtins, FILE* out, int isSourceSet )
{
    const int top = lua_gettop(L);
    lua_getfield(L,inst,"lib_type");
    const int lib_type = isSourceSet ? BS_StaticLib :
                                       strcmp(lua_tostring(L,-1),"shared") == 0 ? BS_DynamicLib : BS_StaticLib;
    lua_pop(L,1);

    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = lib\n"
            "CONFIG -= qt\n"
            "CONFIG += unversioned_libname skip_target_version_ext unversioned_soname\n"
            "CONFIG -= debug_and_release debug_and_release_target\n";
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

    genCommon(L,inst,builtins,lib_type,out);

    forwardDepLibs(L,inst, (isSourceSet ? BS_ForwardSourceSet :
                                          ( lib_type == BS_StaticLib ? BS_ForwardStatic :
                                                                       BS_ForwardShared) ) , out);

    if( !isSourceSet )
    {
        lua_getfield(L,builtins,"#inst");
        lua_getfield(L,-1,"target_os");
        const int win32 = strcmp(lua_tostring(L,-1),"win32") == 0 || strcmp(lua_tostring(L,-1),"winrt") == 0;
        lua_pop(L,2); // target_os, binst

        fwrite("\n",1,1,out);
        addDepLibs(L,inst,builtins,lib_type,out);
        fwrite("\n\n",1,2,out);

        fwrite("\n",1,1,out);
        addLibs(L,inst,lib_type,out,1,win32);
        fwrite("\n\n",1,2,out);

#ifndef BS_QMAKE_HAVE_COPY
        pushLibraryPath(L,inst,builtins,isSourceSet,1,0);
        const int path = lua_gettop(L);
        lua_pushfstring(L,"QMAKE_POST_LINK += $$QMAKE_COPY $$quote(%s) ..\n\n", lua_tostring(L,path));
        fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
        lua_pop(L,2);
#endif
    }

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genExe(lua_State* L, int inst, int builtins, FILE* out )
{
    const int top = lua_gettop(L);
    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = app\n\n"
            "CONFIG += console\n" // avoid that qmake adds /subsys:win on Windows;
                                  // unfortunately it adds /subsys:console, so both a console and a window open
                                  // if ldflags also include /subsys:win; TODO to avoid we need a new field in Executable
            "CONFIG -= app_bundle\n"
            "CONFIG -= qt\n"
            "CONFIG += unversioned_libname skip_target_version_ext unversioned_soname\n"
            "CONFIG -= debug_and_release debug_and_release_target\n";
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

    genCommon(L,inst, builtins, BS_Executable,out);

    lua_getfield(L,builtins,"#inst");
    lua_getfield(L,-1,"target_os");
    const int win32 = strcmp(lua_tostring(L,-1),"win32") == 0 || strcmp(lua_tostring(L,-1),"winrt") == 0;
    lua_pop(L,2); // target_os, binst

    fwrite("\n",1,1,out);
    addDepLibs(L,inst,builtins,BS_Executable,out);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addLibs(L,inst,BS_Executable,out,1,win32);
    fwrite("\n\n",1,2,out);

#ifndef BS_QMAKE_HAVE_COPY
    pushExecutableName(L,inst,builtins,1);
    const int path = lua_gettop(L);
    lua_pushfstring(L,"QMAKE_POST_LINK += $$QMAKE_COPY $$quote(%s) ..\n\n", lua_tostring(L,path));
    fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
    lua_pop(L,2);
#endif

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void concatReplace(lua_State* L, int to, const char* from, int fromLen)
{
    lua_pushvalue(L,to);
    lua_pushlstring(L,from,fromLen);
    lua_concat(L,2);
    lua_replace(L,to);
}

static BSPathStatus apply_arg_expansion(lua_State* L,int inst, const char* source, const char* string)
{
    const int top = lua_gettop(L);

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
                if( t == BS_RootBuildDir )
                {
                    const char* str = "$$root_build_dir";
                    concatReplace(L,out,str,strlen(str));
                }else
                {
                    lua_getfield(L,inst,"#decl");
                    lua_getfield(L,-1,"#qmake");
                    lua_pushfstring(L,"$$root_build_dir/%s", lua_tostring(L,-1));
                    concatReplace(L,out,lua_tostring(L,-1),lua_objlen(L,-1));
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

    const int bottom = lua_gettop(L);
    assert( top+1 ==  bottom);
    return BS_OK;
}

static void genScript(lua_State* L, int inst, FILE* out )
{
    const int top = lua_gettop(L);

    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = aux\n"
            "CONFIG -= qt\n"
            "CONFIG -= debug_and_release debug_and_release_target\n\n";
    fwrite(text,1,strlen(text),out);

    lua_getfield(L,inst,"script");
    const int script = lua_gettop(L);

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    bs_getModuleVar(L,inst,"#fsrdir");
    const int relDir = lua_gettop(L);

    const char* text7 = "SCRIPT = ";
    fwrite(text7,1,strlen(text7),out);
    if( *lua_tostring(L,script) != '/' )
    {
#ifndef BS_QMAKE_GEN_ABS_SOURCE_PATHS
        lua_pushstring(L,"../"); // we're always in a subdir of root_project_dir
        lua_pushstring(L,"$$root_source_dir/");
        addPath(L,relDir,script);
        lua_concat(L,3);
#else
        addPath(L,absDir,file);
#endif
        lua_replace(L,script);
    }
    const char* str = bs_denormalize_path(lua_tostring(L,script));
    fwrite("\"",1,1,out);
    fwrite(str,1,strlen(str),out);
    fwrite("\"\n",1,2,out);

    lua_pushstring(L,"");
    const int str2 = lua_gettop(L);
    lua_getfield(L,inst,"args");
    const int args = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,args); i++ )
    {
        lua_pushvalue(L,str2);
        lua_pushstring(L," \"");
        lua_rawgeti(L,args,i);
        if( apply_arg_expansion(L,inst,0,lua_tostring(L,-1)) != BS_OK )
           luaL_error(L,"cannot do source expansion, invalid placeholders in string: %s", lua_tostring(L,-1));
        lua_replace(L,-2);
        lua_pushstring(L,"\"");
        lua_concat(L,4);
        lua_replace(L,str2);
    }
    lua_pop(L,1); // args

    const char* text3 = "lua.commands = $$lua_path \\\"${QMAKE_FILE_IN}\\\" ";
    fwrite(text3,1,strlen(text3),out);
    fwrite(lua_tostring(L,str2),1,lua_objlen(L,str2),out);
    fwrite("\n",1,1,out);

    const char* text6 = "lua.input = SCRIPT\n"
            "lua.output = ${QMAKE_FILE_BASE}.output\n";
    fwrite(text6,1,strlen(text6),out);


    const char* text4 = "QMAKE_EXTRA_COMPILERS += lua\n";
    fwrite(text4,1,strlen(text4),out);

    lua_pop(L,4); // script, absDir, relDir, str2

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static BSPathStatus apply_source_expansion(lua_State* L,const char* string)
{
    const int top = lua_gettop(L);

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
            if( t == BS_extension || t == BS_completeBaseName || t == BS_fileName )
            {
                    const char* token = "";
                    switch( t )
                    {
                    case BS_extension:
                        token = "${QMAKE_FILE_EXT}";
                        break;
                    case BS_completeBaseName:
                        token = "${QMAKE_FILE_BASE}";
                        break;
                    case BS_fileName:
                        token = "${QMAKE_FILE_BASE}${QMAKE_FILE_EXT}";
                        break;
                    default:
                        assert(0);
                    }
                    const int len2 = strlen(token);
                    concatReplace(L,out,token,len2);
            }else
                return BS_NotSupported;
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

    const int bottom = lua_gettop(L);
    assert( top+1 ==  bottom);
    return BS_OK;
}

static void genCopy(lua_State* L, int inst, FILE* out )
{
    const int top = lua_gettop(L);

    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = aux\n"
            "CONFIG -= qt\n"
            "CONFIG -= debug_and_release debug_and_release_target\n\n";
    fwrite(text,1,strlen(text),out);

    const char* text7 = "COPY_SOURCES +=";
    fwrite(text7,1,strlen(text7),out);
    addSources(L, inst, out,0);

    lua_getfield(L,inst,"use_deps");
    const int use_deps = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,use_deps); i++ )
    {
        lua_rawgeti(L,use_deps,i);
        const char* name = lua_tostring(L,-1);
        if( strcmp(name,"executable") == 0 )
            iterateDeps(L,inst,BS_Executable, 0,out,renderDep);
        else if( strcmp(name,"static_lib") == 0 )
            iterateDeps(L,inst,BS_StaticLib, 0,out,renderDep);
        else if( strcmp(name,"shared_lib") == 0 )
            iterateDeps(L,inst,BS_DynamicLib, 0,out,renderDep);
        else if( strcmp(name,"object_file") == 0 )
            iterateDeps(L,inst,BS_ObjectFiles, 0,out,renderDep);
        lua_pop(L,1);
    }
    lua_pop(L,1);
    fwrite("\n\n",1,2,out);

    const char* text3 = "copy.commands = $$lua_path "
                        "\\\"$$root_project_dir/copy.lua\\\" "
                        "\\\"$$clean_path(${QMAKE_FILE_IN})\\\" \\\"";
    fwrite(text3,1,strlen(text3),out);

    lua_getfield(L,inst,"outputs");
    const int outputs = lua_gettop(L);
    for( i = 1; i <= lua_objlen(L,outputs); i++ )
    {
        lua_pushstring(L,"$$clean_path($$root_build_dir/");
        lua_rawgeti(L,outputs,i);
        if( apply_source_expansion(L,lua_tostring(L,-1)) != BS_OK )
               luaL_error(L,"cannot do source expansion, invalid placeholders in string: %s", lua_tostring(L,-1));
        lua_replace(L,-2);
        lua_concat(L,2);
        fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
        lua_pop(L,1);
        break; // TODO: currently only one output is supported
    }

    fwrite(")\\\"\n",1,4,out);

    const char* text5 = "copy.input = COPY_SOURCES\n";
    fwrite(text5,1,strlen(text5),out);

    const char* text6 = "copy.output = copy.${QMAKE_FILE_BASE}${QMAKE_FILE_EXT}.output\n";
    fwrite(text6,1,strlen(text6),out);

    const char* text4 = "QMAKE_EXTRA_COMPILERS += copy\n";
    fwrite(text4,1,strlen(text4),out);

    lua_pop(L,1); // outputs

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genMoc(lua_State* L, int inst, int builtins, FILE* out )
{
    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = aux\n"
            "CONFIG -= qt\n"
            "CONFIG -= debug_and_release debug_and_release_target\n";
    fwrite(text,1,strlen(text),out);

    fwrite("\n",1,1,out);
    addDefines(L,inst,out,1);
    fwrite("\n\n",1,2,out);

    const char* text7 = "MOC_SOURCES +=";
    fwrite(text7,1,strlen(text7),out);
    addSources(L, inst, out,0);
    fwrite("\n\n",1,2,out);

#if 0
    // since we are no longer dependent on root_build_dir/subdir we can move this declaration to .qmake.conf
    const char* text8 =
            "defineReplace(calc_moc_name){\n" // args: $$1:QMAKE_FILE_IN
            "    result = $$system($$lua_path \\\"$$root_project_dir/moc_name.lua\\\" \\\"$$1\\\")\n"
            // "    message(calc_moc_name $$result)\n"
            "    return($$result) }\n\n";
    fwrite(text8,1,strlen(text8),out);
#endif

#if 1
    lua_getfield(L,inst,"tool_dir");
    const int tool_dir = lua_gettop(L);
    if( !lua_isnil(L,tool_dir) && strcmp(".",lua_tostring(L,tool_dir)) != 0 )
    {
        remapPath(L,builtins,tool_dir);
        lua_pushfstring(L,"moc_path = \\\"%s/moc\\\"\n", bs_denormalize_path(lua_tostring(L,tool_dir)));
        fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
        lua_pop(L,1); // string
    }
    lua_pop(L,1); // tool_dir
#endif

    const char* text3 = "compiler.commands = $$lua_path "
            "\\\"$$root_project_dir/moc.lua\\\" "
            "\\\"$$moc_path\\\" "
            "\\\"${QMAKE_FILE_IN}\\\" "
            "\\\"$$shadowed($$PWD)\\\" "
            "$$join(DEFINES,\" \")\n"; // join is enough, since addDefines quotes if expression
    fwrite(text3,1,strlen(text3),out);

    const char* text5 = "compiler.input = MOC_SOURCES\n";
    fwrite(text5,1,strlen(text5),out);

#if 0
    // NOTE: this is an issue if we have both file.h and file.cpp, because at least on mac it causes
    // the same name twice and an overwrite warning and missing symbols
    const char* text6 = "compiler.output = $$shadowed($$PWD)/moc_${QMAKE_FILE_BASE}.cpp\n";
#else
    const char* text6 = "compiler.output_function = calc_moc_name\n";
#endif
    fwrite(text6,1,strlen(text6),out);

    const char* text4 = "QMAKE_EXTRA_COMPILERS += compiler\n";
    fwrite(text4,1,strlen(text4),out);

    //lua_pop(L,1); // name
}

static void genRcc(lua_State* L, int inst, int builtins, FILE* out )
{
    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = aux\n"
            "CONFIG -= qt\n"
            "CONFIG -= debug_and_release debug_and_release_target\n";
    fwrite(text,1,strlen(text),out);

    const char* text7 = "RCC_SOURCES +=";
    fwrite(text7,1,strlen(text7),out);
    addSources(L, inst, out,0);
    fwrite("\n\n",1,2,out);

#if 1
    lua_getfield(L,inst,"tool_dir");
    const int tool_dir = lua_gettop(L);
    if( !lua_isnil(L,tool_dir) && strcmp(".",lua_tostring(L,tool_dir)) != 0 )
    {
        remapPath(L,builtins,tool_dir);
        lua_pushfstring(L,"rcc_path = \\\"%s/rcc\\\"\n", bs_denormalize_path(lua_tostring(L,tool_dir)));
        fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
        lua_pop(L,1); // string
    }
    lua_pop(L,1); // tool_dir
#endif

    const char* text3 = "compiler.commands = $$rcc_path " // quoting $$rcc_path gives error on windows
            "\\\"${QMAKE_FILE_IN}\\\" "
            "-o \\\"$$shadowed($$PWD)/qrc_${QMAKE_FILE_BASE}.cpp\\\" "
            "-name \"${QMAKE_FILE_BASE}\"";
    fwrite(text3,1,strlen(text3),out);
    fwrite("\n",1,1,out);

    const char* text5 = "compiler.input = RCC_SOURCES\n";
    fwrite(text5,1,strlen(text5),out);

    const char* text6 = "compiler.output = $$shadowed($$PWD)/qrc_${QMAKE_FILE_BASE}.cpp\n";
    fwrite(text6,1,strlen(text6),out);

    const char* text4 = "QMAKE_EXTRA_COMPILERS += compiler\n";
    fwrite(text4,1,strlen(text4),out);
}

static int genproduct(lua_State* L) // arg: prodinst
{
    // Here we now do without the original module structure and instead linearize all modules depth-first
    // under a top-level subdirs project; this has the advantage that we get rid of the intermediate level
    // subdir projects.

    const int prodinst = 1;
    const int top = lua_gettop(L);

    lua_getfield(L,prodinst,"#decl");
    const int decl = lua_gettop(L);

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    const int rootOutDir = lua_gettop(L);

    lua_getfield(L,decl,"#qmake");
    const int name = lua_gettop(L);

    lua_pushfstring(L,"%s/%s/%s.pro", lua_tostring(L,rootOutDir), lua_tostring(L,name), lua_tostring(L,name) );
    const int proPath = lua_gettop(L);

    FILE* out = bs_fopen(bs_denormalize_path(lua_tostring(L,proPath)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,proPath));

    const char* text = "# generated by BUSY, do not modify\n";
    fwrite(text,1,strlen(text),out);

    switch( getClass(L,prodinst,builtins) )
    {
    case BS_LibraryClass:
        genLibrary(L,prodinst, builtins, out, 0);
        break;
    case BS_ExecutableClass:
        genExe(L,prodinst,builtins,out);
        break;
    case BS_SourceSetClass:
        genLibrary(L,prodinst, builtins, out, 1);
        break;
    case BS_MocClass:
        genMoc(L,prodinst, builtins, out);
        break;
    case BS_RccClass:
        genRcc(L,prodinst, builtins, out);
        break;
    case BS_LuaScriptClass:
        genScript(L,prodinst, out);
        break;
#ifdef BS_QMAKE_HAVE_COPY
    case BS_CopyClass:
        genCopy(L,prodinst, out);
        break;
#endif
    default:
        fclose(out);
        assert(0);
        break;
    }

    fclose(out);

    lua_pop(L,6); // decl, builtins, binst, rootOutDir, name, proPath

    const int bottom = lua_gettop(L);
    assert(top == bottom);
    return 0;
}

static int tryrun(lua_State* L, int builtins, const char* cmd)
{
    lua_getfield(L,builtins,"#inst");
    lua_getfield(L,-1,"host_os");
    lua_replace(L,-2);
    const int os = lua_gettop(L);

    lua_pushstring(L,cmd);
    if( strcmp(lua_tostring(L,os),"win32")==0 ||
            strcmp(lua_tostring(L,os),"msdos")==0 ||
            strcmp(lua_tostring(L,os),"winrt")==0 )
        lua_pushstring(L," 2> nul");
    else
        lua_pushstring(L," 2>/dev/null");
    lua_concat(L,2);
    const int success = bs_exec(lua_tostring(L,-1)) == 0;

    lua_pop(L,2);

    return success;
}

int bs_genQmake(lua_State* L) // args: root module def, list of productinst
{
    enum { ROOT = 1, PRODS };
    const int top = lua_gettop(L);

    size_t i;
    lua_createtable(L,0,0);
    const int order = lua_gettop(L);
    for( i = 1; i <= lua_objlen(L,PRODS); i++ )
    {
        lua_pushcfunction(L,mark);
        lua_rawgeti(L,PRODS,i);
        lua_pushvalue(L,order);
        lua_call(L,2,0);
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
    // NOTE: $$PWD and $$shadowed() return empty result here
    // NOTE: compiler.output on the other hand can $$shadowed($$PWD) but ${QMAKE_FILE_BASE} is empty
    // conclusion: we have to declare a specialized version for each Moc instance, see genMoc() above
    const char* text3 = "defineReplace(calc_moc_output){ # args: $$1:QMAKE_FILE_IN\n"
                        "    contains($$1, ^.*\\.(cpp|cxx)$){\n"
                        "        return($$shadowed($$PWD)/$$basename(1).moc)\n"
                        "    }else{\n"
                        "        return($$shadowed($$PWD)/moc_$$basename(1).cpp)\n"
                        "    }\n"
                        "}\n";
    fwrite(text3,1,strlen(text3),out);
#else
    // this version avoids all the qmake issues noted above; result returned by compiler.output_function
    // requires only a filename, not a full filepath! The latter doesn't work at all!
    const char* text12 =
            "defineReplace(calc_moc_name){\n" // args: $$1:QMAKE_FILE_IN
            "    result = $$system($$lua_path \\\"$$root_project_dir/moc_name.lua\\\" \\\"$$1\\\")\n"
            // TEST "    message(calc_moc_name $$result)\n"
            "    return($$result) }\n\n";
    fwrite(text12,1,strlen(text12),out);
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
    lua_pop(L,1); // scriptPath

    lua_pushvalue(L,buildDir);
    lua_pushstring(L,"/moc_name.lua");
    lua_concat(L,2);
    const int script2Path = lua_gettop(L);
    out = bs_fopen(bs_denormalize_path(lua_tostring(L,script2Path)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,script2Path));
    const char* text14 = "-- generated by BUSY, do not modify\n"
            "B = require \"BUSY\"\n"
            "if #arg < 1 then error(\"moc_name.lua expects in-file as argument\") end\n"
            "print(B.moc_name(arg[1]))";
    fwrite(text14,1,strlen(text14),out);
    fclose(out);
    lua_pop(L,1); // script2Path

    lua_pushvalue(L,buildDir);
    lua_pushstring(L,"/copy.lua");
    lua_concat(L,2);
    const int script3Path = lua_gettop(L);
    out = bs_fopen(bs_denormalize_path(lua_tostring(L,script2Path)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,script3Path));
    const char* text15 = "-- generated by BUSY, do not modify\n"
            "B = require \"BUSY\"\n"
            "if #arg < 2 then error(\"copy.lua expects from-path and to-path as arguments\") end\n"
            "print(B.copy(arg[1],arg[2]))";
    fwrite(text15,1,strlen(text15),out);
    fclose(out);
    lua_pop(L,1); // script3Path


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
    int res = bs_makeRelative(lua_tostring(L,buildDir),lua_tostring(L,sourceDir));
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

    const char* text2 = "lua_path = \"";
    fwrite(text2,1,strlen(text2),out);
    bs_thisapp2(L);
    const int thisapp = lua_gettop(L);
    res = bs_makeRelative(lua_tostring(L,buildDir), lua_tostring(L,thisapp));
    if( res == BS_OK )
    {
        lua_pushstring(L,"$$root_project_dir/");
        lua_pushstring(L, bs_global_buffer());
        lua_concat(L,2);
        lua_replace(L,thisapp);
    }
    const char* path = bs_denormalize_path(lua_tostring(L,thisapp));
    fwrite(path,1,strlen(path),out);
    fwrite("\"\n",1,2,out);
    lua_pop(L,1); // thisapp

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
#if 0
        lua_pushfstring(L,"%s/moc", bs_denormalize_path(lua_tostring(L,mocPath)));
        const char* cmd = lua_tostring(L,-1);
        if( !tryrun(L,builtins,cmd) )
        {
            lua_pop(L,1);
            lua_pushstring(L,defaultPath);
        }
        lua_replace(L,mocPath);
#else
        remapPath(L,builtins,mocPath);
        lua_pushfstring(L,"%s/moc", bs_denormalize_path(lua_tostring(L,mocPath)));
        lua_replace(L,mocPath);
#endif
    }
    fwrite(lua_tostring(L,mocPath),1,lua_objlen(L,mocPath),out);
    fwrite("\"\n",1,2,out);

    const char* text8 = "rcc_path = \"";
    fwrite(text8,1,strlen(text8),out);

    lua_getfield(L,binst,"rcc_path");
    const int rccPath = lua_gettop(L);
    const char* defaultRccPath = "$$root_build_dir/rcc";
    if( lua_isnil(L,rccPath) || strcmp(".",lua_tostring(L,rccPath)) == 0 )
    {
        lua_pushstring(L,defaultRccPath);
        lua_replace(L,rccPath);
    }else if( *lua_tostring(L,rccPath) != '/' )
        luaL_error(L,"rcc_path cannot be relative: %s", lua_tostring(L,rccPath));
    else
    {
#if 0
        lua_pushfstring(L,"%s/rcc", bs_denormalize_path(lua_tostring(L,rccPath)));
        const char* cmd = lua_tostring(L,-1);
        if( !tryrun(L,builtins,cmd) )
        {
            lua_pop(L,1);
            lua_pushstring(L,defaultRccPath);
        }
        lua_replace(L,rccPath);
#else
        remapPath(L,builtins,rccPath);
        lua_pushfstring(L,"%s/rcc", bs_denormalize_path(lua_tostring(L,rccPath)));
        lua_replace(L,rccPath);
#endif
    }
    fwrite(lua_tostring(L,rccPath),1,lua_objlen(L,rccPath),out);
    fwrite("\"\n",1,2,out);

    fclose(out);

    lua_pushvalue(L,buildDir);
    lua_pushstring(L,"/Project.pro");
    lua_concat(L,2);
    const int proPath = lua_gettop(L);
    out = bs_fopen(bs_denormalize_path(lua_tostring(L,proPath)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,proPath));

    const char* text7 = "# generated by BUSY, do not modify\n"
            "QT -= core gui\n"
            "TEMPLATE = subdirs\n"
            "CONFIG -= qt\n"
            "CONFIG += ordered\n"
            "SUBDIRS += \\\n";
    fwrite(text7,1,strlen(text7),out);

    size_t len = lua_objlen(L,order);
    for( i = 1; i <= len; i++ )
    {
        lua_rawgeti(L,order,i);
        const int decl = lua_gettop(L);

        lua_getfield(L,decl,"#qmake");
        const int qmake = lua_gettop(L);

        lua_getfield(L,decl,"#owner");
        lua_getfield(L,-1,"#inst"); // owner, modinst
        lua_replace(L,-2); // modinst
        lua_getfield(L,decl,"#name"); // modinst, name
        lua_rawget(L,-2); // modinst, prodinst
        lua_replace(L,-2); // prodinst
        const int prodinst = lua_gettop(L);

        visitDeps(L,prodinst);

        const int cls = getClass(L,prodinst,builtins);
        if( cls == BS_LibraryClass || cls == BS_ExecutableClass || cls == BS_SourceSetClass ||
                cls == BS_MocClass || cls == BS_RccClass || cls == BS_LuaScriptClass
        #ifdef BS_QMAKE_HAVE_COPY
                || cls == BS_CopyClass
        #endif
                )
        {
            fprintf(stdout,"# generating %s\n", lua_tostring(L,qmake));
            fflush(stdout);

            lua_pushfstring(L,"%s/%s", lua_tostring(L,buildDir), lua_tostring(L,qmake));
            const int path = lua_gettop(L);

            if( !bs_exists(lua_tostring(L,path)) )
            {
                if( bs_mkdir(lua_tostring(L,path)) != 0 )
                    luaL_error(L,"error creating directory %s", lua_tostring(L,path));
            }

            fwrite("\t",1,1,out);
            fwrite(lua_tostring(L,qmake),1,lua_objlen(L,qmake),out);
            fwrite(" ",1,1,out);
            if( i < len )
                fwrite("\\",1,1,out);
            fwrite("\n",1,1,out);

            lua_pushcfunction(L,genproduct);
            lua_pushvalue(L,prodinst);
            lua_call(L,1,0);
            lua_pop(L,1); // path
        }else if( cls == BS_LuaScriptForEachClass || cls == BS_MessageClass )
        {
            // TODO: implement LuaScriptForEach
            fprintf(stdout,"# not generating \"%s\" because class \"%s\" is not supported by qmake generator\n",
                    lua_tostring(L,qmake), getClassName(cls));
            fflush(stdout);
        }

        lua_pop(L,3); // decl, qmake, prodinst
    }
    fclose(out);

    lua_pushvalue(L,buildDir);
    lua_pushstring(L,"/dummy.c");
    lua_concat(L,2);
    const int dummyPath = lua_gettop(L);

    out = bs_fopen(bs_denormalize_path(lua_tostring(L,dummyPath)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,dummyPath));
    const char* text9 = "static int dummy() { return 0; }\n";
    fwrite(text9,1,strlen(text9),out);
    fclose(out);

    lua_pop(L,11); // order builtins, binst, buildDir, confPath, sourceDir, mocPath, rccPath, proPath, dummyPath
    assert( top == lua_gettop(L) );
    return 0;
}
