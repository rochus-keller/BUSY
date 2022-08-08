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

#include "bsrunner.h"
#include "bshost.h"
#include "bsparser.h"
#include "lauxlib.h"
#include <assert.h>
#include <string.h>

static int calcdesig(lua_State* L, int decl)
{
    if( decl < 0 )
        decl += lua_gettop(L) + 1;
    lua_getfield(L,decl,"#name");
    const int name = lua_gettop(L);
    lua_getfield(L,decl,"#owner");
    const int module = lua_gettop(L);
    while(!lua_isnil(L,module))
    {
        lua_getfield(L,module,"#name");
        if( lua_isnil(L,-1) )
        {
            lua_pop(L,1);
            break;
        }
        lua_pushstring(L,".");
        lua_pushvalue(L,name);
        lua_concat(L,3);
        lua_replace(L,name);
        lua_getfield(L,module,"#owner");
        lua_replace(L,module);
    }
    lua_pop(L,1); // module
    return 1;
}

static int isa(lua_State* L, int builtins, int cls, const char* what )
{
    lua_getfield(L,builtins,what);
    const int res = bs_isa(L,-1,cls);
    lua_pop(L,1);
    return res;
}

typedef enum BSToolchain {BS_msvc,BS_gcc,BS_clang} BSToolchain;
typedef enum BSLanguage { BS_unknownLang, BS_c, BS_cc, BS_objc, BS_objcc, BS_header } BSLanguage;

static int guessLang(const char* name)
{
    const int len = strlen(name);
    const char* p = name + len - 1;
    while( p != name && *p != '.' )
        p--;

    // see https://gcc.gnu.org/onlinedocs/gcc-4.7.2/gcc/Overall-Options.html
    if( strcmp(p,".c") == 0 )
        return BS_c;
    if( strcmp(p,".h") == 0 )
        return BS_header;
    if( strcmp(p,".cc") == 0 )
        return BS_cc;
    if( strcmp(p,".hh") == 0 )
        return BS_header;
#ifndef _WIN32
    if( strcmp(p,".C") == 0 )
        return BS_cc;
    if( strcmp(p,".H") == 0 )
        return BS_header;
    if( strcmp(p,".HPP") == 0 )
        return BS_header;
    if( strcmp(p,".M") == 0 )
        return BS_objcc;
#endif
    if( strcmp(p,".cpp") == 0 )
        return BS_cc;
    if( strcmp(p,".hpp") == 0 )
        return BS_header;
    if( strcmp(p,".c++") == 0 )
        return BS_cc;
    if( strcmp(p,".h++") == 0 )
        return BS_header;
    if( strcmp(p,".cp") == 0 )
        return BS_cc;
    if( strcmp(p,".hp") == 0 )
        return BS_header;
    if( strcmp(p,".cxx") == 0 )
        return BS_cc;
    if( strcmp(p,".hxx") == 0 )
        return BS_header;
    if( strcmp(p,".m") == 0 )
        return BS_objc;
    if( strcmp(p,".mm") == 0 )
        return BS_objcc;

    return BS_unknownLang;
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

static int getModuleVarFrom(lua_State* L, int inst, const char* name )
{
    const int top = lua_gettop(L);

    lua_getfield(L,inst,"#decl");
    lua_getfield(L,-1,"#owner");
    lua_replace(L,-2);
    lua_getfield(L,-1,name);
    lua_replace(L,-2);

    const int bottom = lua_gettop(L);
    assert( top + 1 == bottom );
    return 1;
}

static void addall(lua_State* L,int inst,int cflags, int cflags_c, int cflags_cc, int cflags_objc, int cflags_objcc,
                        int defines, int includes, int ismsvc)
{
    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int config = lua_gettop(L);
        // TODO: check for circular deps
        addall(L,config,cflags,cflags_c,cflags_cc,cflags_objc,cflags_objcc,defines,includes,ismsvc);
        lua_pop(L,1); // config
    }
    lua_pop(L,1); // configs

    lua_getfield(L,inst,"cflags");
    addflags(L,-1,cflags);
    lua_pop(L,1);

    lua_getfield(L,inst,"cflags_c");
    addflags(L,-1,cflags_c);
    lua_pop(L,1);

    lua_getfield(L,inst,"cflags_cc");
    addflags(L,-1,cflags_cc);
    lua_pop(L,1);

    lua_getfield(L,inst,"cflags_objc");
    addflags(L,-1,cflags_objc);
    lua_pop(L,1);

    lua_getfield(L,inst,"cflags_objcc");
    addflags(L,-1,cflags_objcc);
    lua_pop(L,1);

    getModuleVarFrom(L,inst,"#dir");
    const int absDir = lua_gettop(L);
    const char* s = lua_tostring(L,absDir);

    lua_getfield(L,inst,"include_dirs");
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
        lua_pushvalue(L,includes);
        lua_pushfstring(L," -I\"%s\" ", bs_denormalize_path(lua_tostring(L,path)) );
        lua_concat(L,2);
        lua_replace(L,includes);
        lua_pop(L,1); // path
    }
    lua_pop(L,2); // absDir, incls

    lua_getfield(L,inst,"defines");
    const int defs = lua_gettop(L);
    for( i = 1; i <= lua_objlen(L,defs); i++ )
    {
        lua_rawgeti(L,defs,i);
        lua_pushvalue(L,defines);
        if( strstr(lua_tostring(L,-2),"\\\"") != NULL )
            lua_pushfstring(L," \"-D%s\" ", lua_tostring(L,-2)); // strings can potentially include whitespace, thus quotes
        else
            lua_pushfstring(L," -D%s ", lua_tostring(L,-2));
        lua_concat(L,2);
        lua_replace(L,defines);
        lua_pop(L,1); // def
    }
    lua_pop(L,1); // defs

}

static int getToolchain(lua_State* L, int builtinsInst)
{
    lua_getfield(L,builtinsInst,"target_toolchain");
    int toolchain;
    if( strcmp(lua_tostring(L,-1),"msvc") == 0 )
        toolchain = BS_msvc;
    else if( strcmp(lua_tostring(L,-1),"gcc") == 0 )
        toolchain = BS_gcc;
    else if( strcmp(lua_tostring(L,-1),"clang") == 0 )
        toolchain = BS_clang;
    else
        luaL_error(L,"toolchain not supported: %s",lua_tostring(L,-1) );
    lua_pop(L,1);
    return toolchain;
}

typedef enum BSOutKind { // #kind
    BS_Nothing,
    BS_Mixed, // list of list of the other kinds
    BS_ObjectFiles,
    BS_StaticLib,
    BS_DynamicLib,
    BS_Executable
} BSOutKind;

static void compilesources(lua_State* L, int inst, int builtins)
{
    const int top = lua_gettop(L);

    lua_createtable(L,0,0);
    const int outlist = lua_gettop(L);
    lua_pushinteger(L,BS_ObjectFiles);
    lua_setfield(L,outlist,"#kind");
    lua_pushvalue(L,outlist);
    lua_setfield(L,inst,"#out");

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    const int toolchain = getToolchain(L,binst);

    lua_getfield(L,binst,"root_build_dir");
    const int rootOutDir = lua_gettop(L);

    getModuleVarFrom(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    getModuleVarFrom(L,inst,"#rdir");
    const int relDir = lua_gettop(L);

    lua_pushstring(L,"");
    const int cflags = lua_gettop(L);
    lua_pushstring(L,"");
    const int cflags_c = lua_gettop(L);
    lua_pushstring(L,"");
    const int cflags_cc = lua_gettop(L);
    lua_pushstring(L,"");
    const int cflags_objc = lua_gettop(L);
    lua_pushstring(L,"");
    const int cflags_objcc = lua_gettop(L);
    lua_pushstring(L,"");
    const int defines = lua_gettop(L);
    lua_pushstring(L,"");
    const int includes = lua_gettop(L);

    addall(L,inst,cflags,cflags_c,cflags_cc,cflags_objc,cflags_objcc,defines,includes,toolchain == BS_msvc);

    size_t i;
    lua_getfield(L,inst,"sources");
    const int sources = lua_gettop(L);
    int n = 0;
    for( i = 1; i <= lua_objlen(L,sources); i++ )
    {
        lua_rawgeti(L,sources,i);
        const int file = lua_gettop(L);
        const int lang = guessLang(lua_tostring(L,file));
        if( lang == BS_unknownLang )
            luaL_error(L,"source file type not supported: %s",lua_tostring(L,file));
        if( lang == BS_header )
        {
            lua_pop(L,1);
            continue;
        }

        addPath(L,absDir,file);
        const int src = lua_gettop(L);

        addPath(L,rootOutDir,relDir);
        addPath(L,-1,file);
        if( toolchain == BS_msvc )
            lua_pushstring(L,".obj");
        else
            lua_pushstring(L,".o");
        lua_concat(L,2);
        lua_replace(L,-2);
        const int out = lua_gettop(L);

        lua_pushvalue(L,out);
        lua_rawseti(L,outlist,++n);

        const time_t srcExists = bs_exists(lua_tostring(L,src));
        const time_t outExists = bs_exists(lua_tostring(L,out));
        // check if out is older than source; this is just to avoid total recompile in case of an error,
        // not for development
        if( !outExists || outExists < srcExists )
        {
            switch(toolchain)
            {
            case BS_gcc:
                lua_pushstring(L,"gcc ");
                break;
            case BS_clang:
                lua_pushstring(L,"clang ");
                break;
            case BS_msvc:
                lua_pushstring(L,"cl ");
                break;
            }
            const int cmd = lua_gettop(L);
            lua_pushvalue(L,cmd);
            lua_pushvalue(L,cflags);
            lua_pushvalue(L,includes);
            lua_pushvalue(L,defines);
            switch(lang)
            {
            case BS_c:
                lua_pushstring(L,"");
                break;
            case BS_cc:
                lua_pushvalue(L,cflags_cc);
                break;
            case BS_objc:
                lua_pushvalue(L,cflags_objc);
                break;
            case BS_objcc:
                lua_pushvalue(L,cflags_objcc);
                break;
            }
            switch(toolchain)
            {
            case BS_gcc:
            case BS_clang:
                lua_pushstring(L,"-O2 -c -o ");
                lua_pushfstring(L,"\"%s\" ", bs_denormalize_path(lua_tostring(L,out) ) );
                lua_pushfstring(L,"\"%s\" ", bs_denormalize_path(lua_tostring(L,src) ) );
                break;
            case BS_msvc:
                lua_pushstring(L,"/nologo /O2 /MD /c /Fo");
                lua_pushfstring(L,"\"%s\" ", bs_denormalize_path(lua_tostring(L,out) ) );
                lua_pushfstring(L,"\"%s\" ", bs_denormalize_path(lua_tostring(L,src) ) );
                break;
            }
            lua_concat(L,8);
            lua_replace(L,cmd);
            fprintf(stdout,"%s\n", lua_tostring(L,cmd));
            fflush(stdout);
            if( bs_exec(lua_tostring(L,cmd)) != 0 ) // works for all gcc, clang and cl
            {
                // stderr was already written to the console
                lua_pushnil(L);
                lua_error(L);
            }
            lua_pop(L,1); // cmd
        }
        lua_pop(L,3); // file, source, dest
    }
    lua_pop(L,1); // sources

    lua_pop(L,12); // outlist, binst, rootOutDir...relDir, cflags...includes

    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void addall2(lua_State* L,int inst,int ldflags, int lib_dirs, int lib_names,
                    int lib_files, int frameworks, int ismsvc, int ismac, int iswin)
{
    const int top = lua_gettop(L);

    lua_getfield(L,inst,"configs");
    const int configs = lua_gettop(L);
    size_t i;
    for( i = 1; i <= lua_objlen(L,configs); i++ )
    {
        lua_rawgeti(L,configs,i);
        const int conf = lua_gettop(L);
        // TODO: check for circular deps
        addall2(L,conf,ldflags,lib_dirs,lib_names,lib_files,frameworks, ismsvc, ismac, iswin);
        lua_pop(L,1);
    }
    lua_pop(L,1); // configs

    lua_getfield(L,inst,"ldflags");
    addflags(L,-1,ldflags);
    lua_pop(L,1);

    getModuleVarFrom(L,inst,"#dir");
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
        lua_pushvalue(L,lib_dirs);
        if(ismsvc)
            lua_pushfstring(L," /libpath:\"%s\" ", bs_denormalize_path(lua_tostring(L,path)) );
        else
            lua_pushfstring(L," -L\"%s\" ", bs_denormalize_path(lua_tostring(L,path)) );
        lua_concat(L,2);
        lua_replace(L,lib_dirs);
        lua_pop(L,1); // path
    }
    lua_pop(L,1); // ldirs


    lua_getfield(L,inst,"lib_names");
    const int lnames = lua_gettop(L);
    for( i = 1; i <= lua_objlen(L,lnames); i++ )
    {
        lua_rawgeti(L,lnames,i);
        lua_pushvalue(L,lib_names);
        if(ismsvc)
            lua_pushfstring(L," %s.lib ", lua_tostring(L,-2));
        else
            lua_pushfstring(L," -l%s ", lua_tostring(L,-2));
        lua_concat(L,2);
        lua_replace(L,lib_names);
        lua_pop(L,1); // name
    }
    lua_pop(L,1); // lnames

    if(ismac)
    {
        lua_getfield(L,inst,"frameworks");
        const int fw = lua_gettop(L);
        for( i = 1; i <= lua_objlen(L,lnames); i++ )
        {
            lua_rawgeti(L,fw,i);
            lua_pushvalue(L,frameworks);
            lua_pushfstring(L," -framework %s ", lua_tostring(L,-2));
            lua_concat(L,2);
            lua_replace(L,frameworks);
            lua_pop(L,1); // name
        }
        lua_pop(L,1); // fw
    }

    if( iswin )
    {
        lua_getfield(L,inst,"def_file");
        const int def_file = lua_gettop(L);
        if( !lua_isnil(L,-1) && strcmp(lua_tostring(L,-1),".") != 0 )
        {
            lua_pushvalue(L,ldflags);
            lua_pushstring(L," ");
            if( *lua_tostring(L,def_file) != '/' )
            {
                // relative path
                addPath(L,absDir,def_file);
                lua_replace(L,def_file);
            }
            if(ismsvc)
                lua_pushfstring(L," /def:\"%s\" ", bs_denormalize_path(lua_tostring(L,def_file)) );
            else
                lua_pushfstring(L," \"%s\" ", bs_denormalize_path(lua_tostring(L,def_file)) );
            lua_concat(L,3);
            lua_replace(L,ldflags);
        }
        lua_pop(L,1);// def_file
    }

    //  TODO: lib_files

    lua_pop(L,1); // absDir
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void renderobjectfiles(lua_State* L, int list, FILE* out, int buf, int ismsvc)
{
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
        for( i = lua_objlen(L,list); i >= 1; i-- )
        {
            lua_rawgeti(L,list,i);
            const int sublist = lua_gettop(L);
            renderobjectfiles(L,sublist,out, buf, ismsvc);
            lua_pop(L,1); // sublist
        }
        break;
    case BS_ObjectFiles:
        for( i = lua_objlen(L,list); i >= 1; i-- )
        {
            lua_rawgeti(L,list,i);
            const int path = lua_gettop(L);
            if( buf )
            {
                lua_pushvalue(L,buf);
                lua_pushfstring(L,"\"%s\" ", bs_denormalize_path(lua_tostring(L,path)) );
                lua_concat(L,2);
                lua_replace(L,buf);
            }else
                fprintf(out,"\"%s\" ", bs_denormalize_path(lua_tostring(L,path)) );
            lua_pop(L,1); // path
        }
        break;
    case BS_StaticLib:
    case BS_DynamicLib:
        lua_rawgeti(L,list,1);
        if(0) // apparently this is wrong: if(ismsvc)
              // but TODO we have to link to the .lib even for .dlls: ismsvc)
        {
            if( buf )
            {
                lua_pushvalue(L,buf);
                lua_pushfstring(L,"/implib:\"%s\" ", bs_denormalize_path(lua_tostring(L,-1)) );
                lua_concat(L,2);
                lua_replace(L,buf);
            }else
                fprintf(out,"/implib:\"%s\" ", bs_denormalize_path(lua_tostring(L,-1)) );
        }else
        {
            if( buf )
            {
                lua_pushvalue(L,buf);
                lua_pushfstring(L,"\"%s\" ", bs_denormalize_path(lua_tostring(L,-1)) );
                lua_concat(L,2);
                lua_replace(L,buf);
            }else
                fprintf(out,"\"%s\" ", bs_denormalize_path(lua_tostring(L,-1)) );
        }
        lua_pop(L,1); // path
        break;
    default:
        // ignore
        break;
    }
}

static void link(lua_State* L, int inst, int builtins, int inlist, int kind)
{
    assert( kind == BS_Executable || kind == BS_DynamicLib || kind == BS_StaticLib );

    const int top = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    const int toolchain = getToolchain(L,binst);
    lua_getfield(L,binst,"target_os");
    const int win32 = strcmp(lua_tostring(L,-1),"win32") == 0 || strcmp(lua_tostring(L,-1),"winrt") == 0;
    const int mac = strcmp(lua_tostring(L,-1),"darwin") == 0 || strcmp(lua_tostring(L,-1),"macos") == 0;
    lua_pop(L,1);


    lua_getfield(L,binst,"root_build_dir");
    const int rootOutDir = lua_gettop(L);

    getModuleVarFrom(L,inst,"#rdir");
    const int relDir = lua_gettop(L);

    lua_pushstring(L,"");
    const int ldflags = lua_gettop(L);
    lua_pushstring(L,"");
    const int lib_dirs = lua_gettop(L);
    lua_pushstring(L,"");
    const int lib_names = lua_gettop(L);
    lua_pushstring(L,"");
    const int lib_files = lua_gettop(L);
    lua_pushstring(L,"");
    const int frameworks = lua_gettop(L);

    addall2(L,inst,ldflags,lib_dirs,lib_names,lib_files,frameworks,
            toolchain == BS_msvc || (win32 && toolchain == BS_clang), mac, win32);
            // clang on windows uses the lib.exe compatible llvm-lib.exe tool

    addPath(L,rootOutDir,relDir);
    lua_pushvalue(L,-1);
    lua_pushstring(L,"/");

    if( !win32 && ( kind == BS_DynamicLib || kind == BS_StaticLib ) )
        lua_pushstring(L,"lib"); // if not on Windows prefix the lib name with "lib"
    else
        lua_pushstring(L,"");
    lua_getfield(L,inst,"name");
    if( lua_isnil(L,-1) || lua_objlen(L,-1) == 0 )
    {
        lua_pop(L,1);
        lua_getfield(L,inst,"#decl");
        lua_getfield(L,-1,"#name");
        lua_replace(L,-2);
    }
    lua_concat(L,4);
    lua_replace(L,-2);
    const int outbase = lua_gettop(L);

    lua_pushvalue(L,outbase);
    switch(kind)
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
    const int out = lua_gettop(L);

    lua_pushvalue(L,outbase);
    lua_pushstring(L,".rsp");
    lua_concat(L,2);
    const int rsp = lua_gettop(L);

    int useRsp = 1;

    switch(toolchain)
    {
    case BS_gcc:
        // TODO: since we always use gcc (not g++ and the like) additional flags and libs might be needed
        // see https://stackoverflow.com/questions/172587/what-is-the-difference-between-g-and-gcc
        // g++ on link time is equivalent to gcc -lstdc++ -shared-libgcc
        switch(kind)
        {
        case BS_Executable:
            lua_pushfstring(L,"gcc @\"%s\" -o \"%s\"",
                            bs_denormalize_path(lua_tostring(L,rsp)),
                            bs_denormalize_path(lua_tostring(L,out)) );
            break;
        case BS_DynamicLib:
            lua_pushfstring(L,"gcc %s @\"%s\" -o \"%s\"",
                            (mac ? "-dynamiclib " : "-shared "),
                            bs_denormalize_path(lua_tostring(L,rsp)),
                            bs_denormalize_path(lua_tostring(L,out)) );
            break;
        case BS_StaticLib:
            if( !mac )
                lua_pushfstring(L,"ar r \"%s\" @\"%s\"",
                            bs_denormalize_path(lua_tostring(L,out)),
                            bs_denormalize_path(lua_tostring(L,rsp)) );
            else
            {
                useRsp = 0; // on macs only a few years old ar and gcc version 4 is installed which doesn't support @file
                lua_pushfstring(L,"ar r \"%s\" ", bs_denormalize_path(lua_tostring(L,out)) );
            }
            break;
        }
        break;
    case BS_clang:
        switch(kind)
        {
        case BS_Executable:
            lua_pushfstring(L,"clang @\"%s\" -o \"%s\"",
                            bs_denormalize_path(lua_tostring(L,rsp)),
                            bs_denormalize_path(lua_tostring(L,out)) );
            break;
        case BS_DynamicLib:
            lua_pushfstring(L,"clang %s @\"%s\" -o \"%s\"",
                            (mac ? "-dynamiclib " : "-shared "),
                            bs_denormalize_path(lua_tostring(L,rsp)),
                            bs_denormalize_path(lua_tostring(L,out)) );
            break;
        case BS_StaticLib:
            if( win32 )
                lua_pushfstring(L,"llvm-lib /nologo /out:\"%s\" @\"%s\"",
                            bs_denormalize_path(lua_tostring(L,out)),
                            bs_denormalize_path(lua_tostring(L,rsp)) );
            else
                lua_pushfstring(L,"ar r \"%s\" @\"%s\"",
                                bs_denormalize_path(lua_tostring(L,out)),
                                bs_denormalize_path(lua_tostring(L,rsp)) );
            break;
        }
        break;
    case BS_msvc:
        switch(kind)
        {
        case BS_Executable:
            lua_pushfstring(L,"link /nologo @\"%s\" /out:\"%s\"",
                            bs_denormalize_path(lua_tostring(L,rsp)),
                            bs_denormalize_path(lua_tostring(L,out)) );
            break;
        case BS_DynamicLib:
            lua_pushfstring(L,"link /nologo /dll %s @\"%s\" /out:\"%s\"",
                            (mac ? "-dynamiclib " : "-shared "),
                            bs_denormalize_path(lua_tostring(L,rsp)),
                            bs_denormalize_path(lua_tostring(L,out)) );
            break;
        case BS_StaticLib:
            lua_pushfstring(L,"lib /nologo /out:\"%s\" @\"%s\"",
                            bs_denormalize_path(lua_tostring(L,out)),
                            bs_denormalize_path(lua_tostring(L,rsp)) );
            break;
        }
        break;
    }
    const int cmd = lua_gettop(L);

    lua_createtable(L,0,0);
    const int outlist = lua_gettop(L);
    lua_pushinteger(L,kind);
    lua_setfield(L,outlist,"#kind");
    // TODO: outlist for msvc should include the .lib instead of the .dll
    lua_pushvalue(L,out);
    lua_rawseti(L,outlist,1);
    lua_pushvalue(L,outlist);
    lua_setfield(L,inst,"#out");
    lua_pop(L,1); // outlist


    if( useRsp )
    {
        FILE* f = bs_fopen(bs_denormalize_path(lua_tostring(L,rsp)),"w");
        if( f == NULL )
            luaL_error(L, "cannot open rsp file for writing: %s", lua_tostring(L,rsp));

        renderobjectfiles(L,inlist,f,0, toolchain == BS_msvc);

        fwrite(lua_tostring(L,ldflags),1,lua_objlen(L,ldflags),f);
        fwrite(lua_tostring(L,lib_dirs),1,lua_objlen(L,lib_dirs),f);
        fwrite(lua_tostring(L,lib_names),1,lua_objlen(L,lib_names),f);
        fwrite(lua_tostring(L,lib_files),1,lua_objlen(L,lib_files),f);
        fwrite(lua_tostring(L,frameworks),1,lua_objlen(L,frameworks),f);

        fclose(f);
    }else
    {
        // luaL_Buffer doesn't work; luaL_pushresult produces "attempt to concatenate a table value"
        renderobjectfiles(L,inlist,0,cmd, toolchain == BS_msvc);
        // TODO lib_files
    }

    fprintf(stdout,"%s\n", lua_tostring(L,cmd));
    fflush(stdout);
    if( bs_exec(lua_tostring(L,cmd)) != 0 ) // works for all gcc, clang and cl
    {
        // stderr was already written to the console
        lua_pushnil(L);
        lua_error(L);
    }
    lua_pop(L,1); // cmd

    lua_pop(L,11); // binst, rootOutDir, relDir, ldflags...frameworks, outbase, out, rsp
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void builddeps(lua_State* L, int inst)
{
    const int top = lua_gettop(L);

    lua_getfield(L,inst,"deps");
    const int deps = lua_gettop(L);
    if( lua_isnil(L,deps) )
        return;

    lua_createtable(L,0,0);
    lua_pushinteger(L,BS_Mixed);
    lua_setfield(L,-2,"#kind");
    const int out = lua_gettop(L);

    const int ndeps = lua_objlen(L,deps);
    int nout = 0;
    int i;
    for( i = 1; i <= ndeps; i++ )
    {
        // TODO: check for circular deps
        lua_pushcfunction(L, bs_run);
        lua_rawgeti(L,deps,i);
        lua_call(L,1,1);
        // stack: dep_inst

        lua_getfield(L,-1,"#out");
        lua_replace(L,-2);
        // stack: subout

        const int subout = lua_gettop(L);
        lua_getfield(L,-1,"#kind");
        const int k = lua_tointeger(L,-1);
        lua_pop(L,1);

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
        }else
            lua_rawseti(L,out,++nout); // eats subout
    }
    lua_setfield(L,inst,"#out");
    lua_pop(L,1); // deps
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void library(lua_State* L,int inst, int cls, int builtins)
{
    lua_getfield(L,inst,"#out");
    const int mixed = lua_gettop(L);
    assert( lua_istable(L,mixed) );
    compilesources(L,inst,builtins);
    lua_getfield(L,inst,"#out");
    if( lua_objlen(L,mixed) == 0 )
        lua_replace(L,mixed);
    else
        lua_rawseti(L,mixed,lua_objlen(L,mixed)+1);
    lua_getfield(L,inst,"lib_type");
    if( strcmp(lua_tostring(L,-1),"shared") == 0 )
        link(L,inst,builtins,mixed,BS_DynamicLib);
    else
        link(L,inst,builtins,mixed,BS_StaticLib);

    lua_pop(L,2); // out, lib_type
}

static void executable(lua_State* L,int inst, int cls, int builtins)
{
    lua_getfield(L,inst,"#out");
    const int mixed = lua_gettop(L);
    assert( lua_istable(L,mixed) );
    compilesources(L,inst,builtins);
    lua_getfield(L,inst,"#out");
    if( lua_objlen(L,mixed) == 0 )
        lua_replace(L,mixed);
    else
        lua_rawseti(L,mixed,lua_objlen(L,mixed)+1);
    link(L,inst,builtins,mixed,BS_Executable);
    lua_pop(L,1); // mixed
}

static void sourceset(lua_State* L,int inst, int cls, int builtins)
{
    lua_getfield(L,inst,"#out");
    const int mixed = lua_gettop(L);
    assert( lua_istable(L,mixed) );
    compilesources(L,inst,builtins);
    if( lua_objlen(L,mixed) == 0 )
        lua_pop(L,1); // mixed
    else
    {
        lua_getfield(L,inst,"#out"); // result of compilesources
        lua_rawseti(L,mixed,lua_objlen(L,mixed)+1); // append it to mixed
        lua_setfield(L,inst,"#out"); // set mixed again as inst.#out
    }
}

static void group(lua_State* L,int inst, int cls, int builtins)
{
    // NOP. deps were already built and result handed to inst.#out
}

static void config(lua_State* L,int inst, int cls, int builtins)
{
    // NOP
}

int bs_thisapp2(lua_State *L)
{
    // see https://stackoverflow.com/questions/933850/how-do-i-find-the-location-of-the-executable-in-c
    const int res = bs_thisapp();
    if( res == BS_NOP )
    {
        lua_getglobal(L,"#prog");
        const char* path = lua_tostring(L,-1);
        if( path[0] == '/' || path[0] == '\\' )
            return 1;
        if( strchr(path,'/') != 0 || strchr(path,'\\') != 0 )
        {
            if( bs_cwd() == BS_OK )
                lua_pushstring(L, bs_global_buffer());
            else
                luaL_error(L,"getcwd: received non supported path from OS");
            addPath(L,-1,-2);
            // stack: #prog, cwd, abspath
            lua_replace(L,-3);
            lua_pop(L,1);
            return 1;
        }else
            luaL_error(L,"thisapp: cannot determine path of current application");
    }else if( res == BS_OK )
        lua_pushstring(L, bs_global_buffer());
    else
        luaL_error(L,"thisapp: received non supported path from OS");
    return 1;
}

static void script(lua_State* L,int inst, int cls, int builtins)
{
    const int top = lua_gettop(L);

    lua_createtable(L,0,0);
    lua_pushinteger(L,BS_Nothing);
    lua_setfield(L,-2,"#kind");
    lua_setfield(L,inst,"#out");

    getModuleVarFrom(L,inst,"#dir");
    const int absDir = lua_gettop(L);

    lua_getfield(L,inst,"script");
    const int script = lua_gettop(L);
    if( *lua_tostring(L,script) != '/' )
    {
        // relative path
        addPath(L,absDir,script);
        lua_replace(L,script);
    }

    bs_thisapp2(L);
    const int app = lua_gettop(L);

    lua_pushstring(L,"");
    const int args = lua_gettop(L);

    lua_getfield(L,inst,"args");
    addflags(L,-1,args);
    lua_pop(L,1);

    lua_pushfstring(L, "%s %s %s", bs_denormalize_path(lua_tostring(L,app) ),
                    bs_denormalize_path(lua_tostring(L,script) ),
                    lua_tostring(L,args) );
    const int cmd = lua_gettop(L);

    fprintf(stdout,"%s\n", lua_tostring(L,cmd));
    fflush(stdout);
    if( bs_exec(lua_tostring(L,cmd)) != 0 )
    {
        // stderr was already written to the console
        lua_pushnil(L);
        lua_error(L);
    }

    lua_pop(L,5); // abDir, script, app, args, cmd
    const int bottom = lua_gettop(L);
    assert( top == bottom );
}

static void foreach(lua_State* L,int inst, int cls, int builtins)
{
    luaL_error(L,"'LuaScriptForeach' not yet implemented");
}

static void copy(lua_State* L,int inst, int cls, int builtins)
{
    luaL_error(L,"'Copy' not yet implemented");
}

int bs_createBuildDirs(lua_State* L) // lua function; params: rootModuleDef, rootPath
{
    enum { rootModule = 1, rootPath = 2 };
    if( !bs_exists(lua_tostring(L,rootPath)) )
    {
        if( bs_mkdir(lua_tostring(L,rootPath)) != 0 )
            luaL_error(L,"error creating directory %s", lua_tostring(L,-1));
        fflush(stdout);
    }
    size_t i;
    for( i = 1; i <= lua_objlen(L,rootModule); i++ )
    {
        lua_pushcfunction(L, bs_createBuildDirs);

        lua_rawgeti(L,rootModule,i);
        const int module = lua_gettop(L);

        lua_getfield(L,module,"#kind");
        const int k = lua_tointeger(L,-1);
        lua_pop(L,1);

        if( k == BS_ModuleDef )
        {
            lua_pushvalue(L,rootPath);
            lua_pushstring(L,"/");
            lua_getfield(L,module,"#dirname");
            lua_concat(L,3);
            lua_call(L,2,0);
        }else
            lua_pop(L,2); // func, module
    }
    return 0;
}

int bs_run(lua_State* L) // args: productinst, returns: inst
{
    const int inst = 1;

    lua_getfield(L,inst,"#out");
    const int built = !lua_isnil(L,-1);
    lua_pop(L,1);
    if( built )
        return 1; // we're already built

    lua_getmetatable(L,inst);
    const int cls = lua_gettop(L);

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getfield(L,cls,"#name");
    const char* name = lua_tostring(L,-1);

    builddeps(L,inst);

    lua_getfield(L,inst,"#decl");
    calcdesig(L,-1);
    fprintf(stdout,"# building %s %s\n",name,lua_tostring(L,-1));
    fflush(stdout);
    lua_pop(L,2); // decl, name

    // use isa instead of strcmp so that users can subclass the built-in classes
    if( isa( L, builtins, cls, "Library" ) )
        library(L,inst,cls,builtins);
    else if( isa( L, builtins, cls, "Executable") )
        executable(L,inst,cls,builtins);
    else if( isa( L, builtins, cls, "SourceSet") )
        sourceset(L,inst,cls,builtins);
    else if( isa( L, builtins, cls, "Group") )
        group(L,inst,cls,builtins);
    else if( isa( L, builtins, cls, "Config") )
        config(L,inst,cls,builtins);
    else if( isa( L, builtins, cls, "LuaScript") )
        script(L,inst,cls,builtins);
    else if( isa( L, builtins, cls, "LuaScriptForeach") )
        foreach(L,inst,cls,builtins);
    else if( isa( L, builtins, cls, "Copy") )
        copy(L,inst,cls,builtins);
    else
        luaL_error(L,"don't know how to build instances of class '%s'", name);

    lua_pop(L,3); // cls, builtins, name
    return 1; // inst
}
