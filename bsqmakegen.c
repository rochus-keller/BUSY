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
    if( lua_isnil(L,out) )
    {
        lua_createtable(L,0,0);
        lua_replace(L,out);
        lua_pushvalue(L,out);
        lua_setfield(L,inst,"#out");
    }

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

static void mocDep(lua_State* L, int inst)
{
    const int top = lua_gettop(L);
    visitDeps(L,inst);

    lua_getfield(L,inst,"#out");
    const int out = lua_gettop(L);
    if( lua_isnil(L,out) )
    {
        lua_createtable(L,0,0);
        lua_replace(L,out);
        lua_pushvalue(L,out);
        lua_setfield(L,inst,"#out");
    }

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
    if( lua_isnil(L,out) )
    {
        lua_createtable(L,0,0);
        lua_replace(L,out);
        lua_pushvalue(L,out);
        lua_setfield(L,inst,"#out");
    }

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
        mocDep(L,inst);
    else if( isa( L, builtins, cls, "Rcc") )
        rccDep(L,inst);
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

static int addSources(lua_State* L, int inst, FILE* out)
{
    const int top = lua_gettop(L);
    const char* text = "SOURCES +="; // TODO: OBJECTIVE_SOURCES for .mm
    fwrite(text,1,strlen(text),out);

    s_sourceCount = 0;
    iterateDeps(L,inst,BS_SourceFiles, 0,out,renderDep);
    int n = s_sourceCount; // RISK this works as long we don't use threads

    lua_getfield(L,inst,"sources");
    const int sources = lua_gettop(L);

    bs_getModuleVar(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    size_t i;
    const size_t len = lua_objlen(L,sources);
    n += len;
    for( i = 1; i <= len; i++ )
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

    return n;
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
                isMoc = isa( L, builtins, cls, "Moc");
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
            addPath(L,absDir,include);
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
                lua_pushfstring(L,"$$root_build_dir/%s", lua_tostring(L, lua_gettop(L) - n + 1));
                renderQuotedPath(L,-1,out);
                lua_pop(L,1);
            }
            lua_pop(L,n);
        }else
            renderQuotedPath(L,include,out);

        lua_pop(L,1); // include
    }

    lua_pop(L,3); // includes, absDir, rootBuildDir

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
    const int binst = lua_gettop(L);
    const int ts = bs_getToolchain(L, binst );
    lua_pop(L,1); // binst

    if( kind == BS_DynamicLib )
    {
        iterateDeps(L,inst,BS_ObjectFiles,1,out,renderDep);
    }

    if( ( kind == BS_DynamicLib || kind == BS_Executable ) && ( ts == BS_gcc || ts == BS_clang ) )
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

    if( ( kind == BS_DynamicLib || kind == BS_Executable ) && ( ts == BS_gcc || ts == BS_clang ) )
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

        lua_getfield(L,inst,"lib_dirs");
        const int ldirs = lua_gettop(L);

        for( i = 1; i <= lua_objlen(L,ldirs); i++ )
        {
            lua_rawgeti(L,ldirs,i);
            const int path = lua_gettop(L);
            if( *lua_tostring(L,-1) != '/' )
            {
                // relative path
                addPath(L,absDir,path);
                lua_replace(L,path);
            }
            if(ismsvc)
                lua_pushfstring(L,"/libpath:\"%s\"", bs_denormalize_path(lua_tostring(L,path)) );
            else
                lua_pushfstring(L,"-L\"%s\"", bs_denormalize_path(lua_tostring(L,path)) );
            lua_concat(L,2);
            fwrite(s_listFill1,1,strlen(s_listFill1),out);
            fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
            fwrite("\"",1,1,out);
            lua_pop(L,1); // path
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
        lua_pop(L,3); // ldirs, absDir, lnames
        // TODO: frameworks, def_file, lib_files
    }


    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genCommon(lua_State* L, int inst, int builtins, int kind, FILE* out )
{
    fwrite("\n",1,1,out);
    addDefines(L,inst,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    addIncludes(L,inst, builtins,out,1);
    fwrite("\n\n",1,2,out);

    fwrite("\n",1,1,out);
    const int nSources = addSources(L,inst,out);
    if( nSources == 0 )
    {
        lua_getfield(L,builtins,"#inst");
        lua_getfield(L,-1,"root_build_dir");
        lua_pushfstring(L,"%s/dummy.c\"",lua_tostring(L,-1));
        fwrite(s_listFill1,1,strlen(s_listFill1),out);
        const char* path = bs_denormalize_path(lua_tostring(L,-1));
        fwrite(path,1,strlen(path),out);
        lua_pop(L,3);
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
    // TODO should we consider #ctdefaults here?
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

        pushLibraryPath(L,inst,builtins,isSourceSet,1,0);
        const int path = lua_gettop(L);
        lua_pushfstring(L,"QMAKE_POST_LINK += $$QMAKE_COPY $$quote(%s) ..\n\n", lua_tostring(L,path));
        fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
        lua_pop(L,2);
    }

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genExe(lua_State* L, int inst, int builtins, FILE* out )
{
    // TODO should we consider #ctdefaults here?
    const int top = lua_gettop(L);
    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = app\n\n"
            "CONFIG += console\n" // avoid that qmake adds /subsys:win on Windows;
                                  // unfortunately it adds /subsys:console, so both a console and a window open
                                  // if ldflags also include /subsys:win; TODO to avoid we need a new field in Executable
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

    pushExecutableName(L,inst,builtins,1);
    const int path = lua_gettop(L);
    lua_pushfstring(L,"QMAKE_POST_LINK += $$QMAKE_COPY $$quote(%s) ..\n\n", lua_tostring(L,path));
    fwrite(lua_tostring(L,-1),1,lua_objlen(L,-1),out);
    lua_pop(L,2);

    const int bottom = lua_gettop(L);
    assert( top ==  bottom);
}

static void genAux(lua_State* L, int inst, FILE* out )
{

    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = aux\n"
            "CONFIG -= qt\n"
            "CONFIG -= debug_and_release debug_and_release_target\n";
    fwrite(text,1,strlen(text),out);
}

static void genMoc(lua_State* L, int inst, FILE* out )
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

static void genRcc(lua_State* L, int inst, FILE* out )
{
    const char* text =
            "QT -= core gui\n"
            "TEMPLATE = aux\n"
            "CONFIG -= qt\n"
            "CONFIG -= debug_and_release debug_and_release_target\n";
    fwrite(text,1,strlen(text),out);

    const char* text7 = "RCC_SOURCES +=";
    fwrite(text7,1,strlen(text7),out);
    addSources2(L, inst, out);
    fwrite("\n\n",1,2,out);

    const char* text3 = "compiler.commands = "
            "\\\"$$rcc_path\\\" "
            "\\\"${QMAKE_FILE_IN}\\\" "
            "-o \\\"$$shadowed($$PWD)/qrc_${QMAKE_FILE_BASE}.cpp\\\" "
            "-name \"${QMAKE_FILE_BASE}\"";
    fwrite(text3,1,strlen(text3),out);
    fwrite("\n",1,1,out);

    const char* text5 = "compiler.input = RCC_SOURCES";
    fwrite(text5,1,strlen(text5),out);
    fwrite("\n",1,1,out);

    const char* text6 = "compiler.output = $$shadowed($$PWD)/qrc_${QMAKE_FILE_BASE}.cpp";
    fwrite(text6,1,strlen(text6),out);
    fwrite("\n",1,1,out);

    const char* text4 = "QMAKE_EXTRA_COMPILERS += compiler";
    fwrite(text4,1,strlen(text4),out);
    fwrite(" \n",1,1,out);
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

    visitDeps(L,prodinst);

    lua_getmetatable(L,prodinst);
    const int cls = lua_gettop(L);

    if( isa( L, builtins, cls, "Library" ) )
        genLibrary(L,prodinst, builtins, out, 0);
    else if( isa( L, builtins, cls, "Executable") )
        genExe(L,prodinst,builtins,out);
    else if( isa( L, builtins, cls, "SourceSet") )
        genLibrary(L,prodinst, builtins, out, 1);
    else if( isa( L, builtins, cls, "Group") )
        genAux(L,prodinst, out);
    else if( isa( L, builtins, cls, "Config") )
        genAux(L,prodinst, out);
    else if( isa( L, builtins, cls, "LuaScript") )
        genAux(L,prodinst, out); // TODO;
    else if( isa( L, builtins, cls, "LuaScriptForeach") )
        genAux(L,prodinst, out); // TODO
    else if( isa( L, builtins, cls, "Copy") )
        genAux(L,prodinst, out); // TODO
    else if( isa( L, builtins, cls, "Message") )
        genAux(L,prodinst, out); // TODO
    else if( isa( L, builtins, cls, "Moc") )
        genMoc(L,prodinst, out);
    else if( isa( L, builtins, cls, "Rcc") )
        genRcc(L,prodinst, out);
    else
    {
        fclose(out);
        lua_getfield(L,cls,"#name");
        luaL_error(L,"don't know how to build instances of class '%s'", lua_tostring(L,-1));
    }

    fclose(out);

    lua_pop(L,7); // decl, builtins, binst, rootOutDir, name, proPath, cls

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
        if( !tryrun(L,builtins,cmd) )
            cmd = defaultPath;
        lua_pushstring(L,cmd);
        lua_replace(L,mocPath);
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
        const char* cmd = bs_denormalize_path(lua_tostring(L,rccPath));
        if( !tryrun(L,builtins,cmd) )
            cmd = defaultRccPath;
        lua_pushstring(L,cmd);
        lua_replace(L,rccPath);
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
        fprintf(stdout,"# generating %s\n", lua_tostring(L,-1));
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

        lua_getfield(L,decl,"#owner");
        lua_getfield(L,-1,"#inst");
        lua_replace(L,-2);
        const int modinst = lua_gettop(L);
        lua_pushcfunction(L,genproduct);
        lua_getfield(L,decl,"#name");
        lua_rawget(L,modinst);
        lua_call(L,1,0);

        lua_pop(L,4); // prodinst, qmake, path, modinst
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

    lua_pop(L,12); // order builtins, binst, buildDir, confPath, sourceDir, scriptPath, mocPath, rccPath, proPath, dummyPath
    assert( top == lua_gettop(L) );
    return 0;
}
