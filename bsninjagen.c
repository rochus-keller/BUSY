/*
* Copyright 2026 Rochus Keller <mailto:me@rochus-keller.ch>
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

#include "bsninjagen.h"
#include "bsvisitor.h"
#include "bshost.h"
#include "bsparser.h"
#include "lauxlib.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


typedef struct DynBuf {
    char* data;
    size_t len;
    size_t alloc;
} DynBuf;

static void db_init(DynBuf* b)
{
    b->data = 0;
    b->len = 0;
    b->alloc = 0;
}

static void db_free(DynBuf* b)
{
    free(b->data);
    b->data = 0;
    b->len = 0;
    b->alloc = 0;
}

static void db_clear(DynBuf* b)
{
    b->len = 0;
}

static void db_append(DynBuf* b, const char* s)
{
    const size_t slen = strlen(s);
    if( slen == 0 )
        return;
    if( b->len + slen + 1 > b->alloc )
    {
        size_t newalloc = b->alloc == 0 ? 256 : b->alloc;
        while( newalloc < b->len + slen + 1 )
            newalloc *= 2;
        b->data = (char*)realloc(b->data, newalloc);
        b->alloc = newalloc;
    }
    memcpy(b->data + b->len, s, slen);
    b->len += slen;
    b->data[b->len] = 0;
}

static const char* db_str(DynBuf* b)
{
    if( b->data == 0 )
        return "";
    return b->data;
}

/* set of already-emitted build outputs, used to deduplicate build statements. A product such as a Copy
   can be reached through several dependency paths (e.g. both the executable and a Rcc product depend on
   the copied tool), and unlike Library/SourceSet/Moc it is not marked built in the visitor, so its build
   operation is replayed once per path. ninja rejects two rules generating the same output, so we skip a
   build statement whose output was already written. */

typedef struct StrSet {
    char** items;
    size_t len;
    size_t alloc;
} StrSet;

static void strset_init(StrSet* s)
{
    s->items = 0;
    s->len = 0;
    s->alloc = 0;
}

static void strset_free(StrSet* s)
{
    size_t i;
    for( i = 0; i < s->len; i++ )
        free(s->items[i]);
    free(s->items);
    s->items = 0;
    s->len = 0;
    s->alloc = 0;
}

static int strset_contains(StrSet* s, const char* str)
{
    size_t i;
    for( i = 0; i < s->len; i++ )
    {
        if( strcmp(s->items[i],str) == 0 )
            return 1;
    }
    return 0;
}

static int strset_seen(StrSet* s, const char* str)
{
    /* returns 1 if str was already present, otherwise adds it and returns 0 */
    size_t i;
    for( i = 0; i < s->len; i++ )
    {
        if( strcmp(s->items[i],str) == 0 )
            return 1;
    }
    if( s->len + 1 > s->alloc )
    {
        s->alloc = s->alloc == 0 ? 64 : s->alloc * 2;
        s->items = (char**)realloc(s->items, s->alloc * sizeof(char*));
    }
    s->items[s->len] = (char*)malloc(strlen(str)+1);
    strcpy(s->items[s->len], str);
    s->len++;
    return 0;
}

static void db_append_escaped(DynBuf* b, const char* s)
{
    /* ninja path escaping: $ -> $$, space -> $ , colon -> $: */
    const char* p;
    for( p = s; *p; p++ )
    {
        switch( *p )
        {
        case '$':
            db_append(b,"$$");
            break;
        case ' ':
            db_append(b,"$ ");
            break;
        case ':':
            db_append(b,"$:");
            break;
        case '\n':
            db_append(b,"$\n");
            break;
        default:
            {
                char tmp[2];
                tmp[0] = *p;
                tmp[1] = 0;
                db_append(b,tmp);
            }
            break;
        }
    }
}


typedef struct NinjaCtx {
    FILE* out;
    BSBuildOperation op;
    int toolchain;
    int os;
    DynBuf command;    /* the tool command (gcc, ar, moc, etc.) */
    DynBuf infiles;    /* space-separated escaped input files for build line */
    DynBuf outfile;    /* escaped output file for build line */
    DynBuf cflags;     /* compiler flags */
    DynBuf defines;    /* preprocessor defines (already prefixed) */
    DynBuf includes;   /* include dirs (already prefixed) */
    DynBuf ldflags;    /* linker flags */
    DynBuf lib_dirs;   /* library search dirs (already prefixed) */
    DynBuf lib_names;  /* library names (already prefixed) */
    DynBuf lib_files;  /* explicit lib file paths */
    DynBuf frameworks; /* -framework flags */
    DynBuf deffile;    /* /DEF: file for msvc */
    DynBuf args;       /* script arguments */
    DynBuf name;       /* rcc resource name */
    DynBuf mocargs;    /* extra moc arguments (e.g. -p/-b private header) */
    DynBuf depfiles;   /* output files of dependency products (implicit deps) */
    StrSet emitted;    /* build outputs already written, for deduplication */
} NinjaCtx;

static void ctx_init(NinjaCtx* ctx)
{
    ctx->out = 0;
    ctx->op = BS_Compile;
    ctx->toolchain = 0;
    ctx->os = 0;
    db_init(&ctx->command);
    db_init(&ctx->infiles);
    db_init(&ctx->outfile);
    db_init(&ctx->cflags);
    db_init(&ctx->defines);
    db_init(&ctx->includes);
    db_init(&ctx->ldflags);
    db_init(&ctx->lib_dirs);
    db_init(&ctx->lib_names);
    db_init(&ctx->lib_files);
    db_init(&ctx->frameworks);
    db_init(&ctx->deffile);
    db_init(&ctx->args);
    db_init(&ctx->name);
    db_init(&ctx->mocargs);
    db_init(&ctx->depfiles);
    strset_init(&ctx->emitted);
}

static void ctx_free(NinjaCtx* ctx)
{
    db_free(&ctx->command);
    db_free(&ctx->infiles);
    db_free(&ctx->outfile);
    db_free(&ctx->cflags);
    db_free(&ctx->defines);
    db_free(&ctx->includes);
    db_free(&ctx->ldflags);
    db_free(&ctx->lib_dirs);
    db_free(&ctx->lib_names);
    db_free(&ctx->lib_files);
    db_free(&ctx->frameworks);
    db_free(&ctx->deffile);
    db_free(&ctx->args);
    db_free(&ctx->name);
    db_free(&ctx->mocargs);
    db_free(&ctx->depfiles);
    strset_free(&ctx->emitted);
}

static void ctx_clear(NinjaCtx* ctx)
{
    db_clear(&ctx->command);
    db_clear(&ctx->infiles);
    db_clear(&ctx->outfile);
    db_clear(&ctx->cflags);
    db_clear(&ctx->defines);
    db_clear(&ctx->includes);
    db_clear(&ctx->ldflags);
    db_clear(&ctx->lib_dirs);
    db_clear(&ctx->lib_names);
    db_clear(&ctx->lib_files);
    db_clear(&ctx->frameworks);
    db_clear(&ctx->deffile);
    db_clear(&ctx->args);
    db_clear(&ctx->name);
    db_clear(&ctx->mocargs);
    db_clear(&ctx->depfiles);
}


static int ninja_begin(BSBuildOperation op, const char* command, int toolchain, int os, void* data)
{
    NinjaCtx* ctx = (NinjaCtx*)data;

    if( op == BS_EnteringProduct )
    {
        fprintf(ctx->out, "\n# %s\n", command);
        return 0;
    }

    ctx_clear(ctx);
    ctx->op = op;
    ctx->toolchain = toolchain;
    ctx->os = os;
    db_append(&ctx->command, command);

    return 0;
}

static void ninja_param(BSBuildParam p, const char* value, void* data)
{
    NinjaCtx* ctx = (NinjaCtx*)data;

    switch(p)
    {
    case BS_infile:
        if( ctx->infiles.len > 0 )
            db_append(&ctx->infiles, " ");
        db_append_escaped(&ctx->infiles, value);
        break;
    case BS_outfile:
        if( ctx->outfile.len > 0 )
            db_append(&ctx->outfile, " ");
        db_append_escaped(&ctx->outfile, value);
        break;
    case BS_cflag:
        db_append(&ctx->cflags, " ");
        db_append(&ctx->cflags, value);
        break;
    case BS_define:
        db_append(&ctx->defines, " ");
        if( ctx->toolchain == BS_msvc )
        {
            db_append(&ctx->defines, "/D");
            db_append(&ctx->defines, value);
        }else
        {
            db_append(&ctx->defines, "-D");
            if( strstr(value,"\\\"") != NULL )
            {
                /* strings can potentially include whitespace or shell
                   metacharacters, thus quotes (same convention as bsrunner) */
                db_append(&ctx->defines, "\"");
                db_append(&ctx->defines, value);
                db_append(&ctx->defines, "\"");
            }else
                db_append(&ctx->defines, value);
        }
        break;
    case BS_include_dir:
        db_append(&ctx->includes, " ");
        if( ctx->toolchain == BS_msvc )
        {
            db_append(&ctx->includes, "/I\"");
            db_append(&ctx->includes, value);
            db_append(&ctx->includes, "\"");
        }else
        {
            db_append(&ctx->includes, "-I\"");
            db_append(&ctx->includes, value);
            db_append(&ctx->includes, "\"");
        }
        break;
    case BS_depfile:
        if( ctx->depfiles.len > 0 )
            db_append(&ctx->depfiles, " ");
        db_append_escaped(&ctx->depfiles, value);
        break;
    case BS_ldflag:
        db_append(&ctx->ldflags, " ");
        db_append(&ctx->ldflags, value);
        break;
    case BS_lib_dir:
        db_append(&ctx->lib_dirs, " ");
        if( ctx->toolchain == BS_msvc )
        {
            db_append(&ctx->lib_dirs, "/libpath:\"");
            db_append(&ctx->lib_dirs, value);
            db_append(&ctx->lib_dirs, "\"");
        }else
        {
            db_append(&ctx->lib_dirs, "-L\"");
            db_append(&ctx->lib_dirs, value);
            db_append(&ctx->lib_dirs, "\"");
        }
        break;
    case BS_lib_name:
        db_append(&ctx->lib_names, " ");
        if( ctx->toolchain == BS_msvc )
        {
            db_append(&ctx->lib_names, value);
            db_append(&ctx->lib_names, ".lib");
        }else
        {
            db_append(&ctx->lib_names, "-l");
            db_append(&ctx->lib_names, value);
        }
        break;
    case BS_lib_file:
        db_append(&ctx->lib_files, " \"");
        db_append(&ctx->lib_files, value);
        db_append(&ctx->lib_files, "\"");
        break;
    case BS_framework:
        db_append(&ctx->frameworks, " -framework ");
        db_append(&ctx->frameworks, value);
        break;
    case BS_defFile:
        db_clear(&ctx->deffile);
        db_append(&ctx->deffile, value);
        break;
    case BS_name:
        db_clear(&ctx->name);
        db_append(&ctx->name, value);
        break;
    case BS_arg:
        db_append(&ctx->args, " \"");
        db_append(&ctx->args, value);
        db_append(&ctx->args, "\"");
        break;
    case BS_moc_arg:
        db_append(&ctx->mocargs, " \"");
        db_append(&ctx->mocargs, value);
        db_append(&ctx->mocargs, "\"");
        break;
    }
}


static void write_compile(NinjaCtx* ctx)
{
    /* emits a compile build statement.
       The rule uses $cc, $cflags, $depfile for gcc-style, or $cc, $cflags for msvc. */
    FILE* out = ctx->out;
    const int ismsvc = (ctx->toolchain == BS_msvc);

    fprintf(out, "build %s: %s %s\n",
            db_str(&ctx->outfile),
            ismsvc ? "cc_msvc" : "cc_gcc",
            db_str(&ctx->infiles));
    fprintf(out, "  cc = %s\n", db_str(&ctx->command));
    fprintf(out, "  cflags =%s%s%s\n",
            db_str(&ctx->cflags),
            db_str(&ctx->defines),
            db_str(&ctx->includes));
}


static void write_link_exe(NinjaCtx* ctx)
{
    /* emits a link-executable build statement. */
    FILE* out = ctx->out;
    const int ismsvc = (ctx->toolchain == BS_msvc);

    fprintf(out, "build %s: link_exe %s\n",
            db_str(&ctx->outfile),
            db_str(&ctx->infiles));
    fprintf(out, "  linker = %s\n", db_str(&ctx->command));
    fprintf(out, "  ldflags =%s%s%s%s%s\n",
            db_str(&ctx->ldflags),
            db_str(&ctx->lib_dirs),
            db_str(&ctx->lib_names),
            db_str(&ctx->lib_files),
            db_str(&ctx->frameworks));
    if( ismsvc )
        fprintf(out, "  ismsvc = 1\n");
}

static void write_link_dll(NinjaCtx* ctx)
{
    /* emits a link-shared-library build statement. */
    FILE* out = ctx->out;
    const int ismsvc = (ctx->toolchain == BS_msvc);
    const int ismac = (ctx->os == BS_mac);

    fprintf(out, "build %s: link_dll %s\n",
            db_str(&ctx->outfile),
            db_str(&ctx->infiles));
    fprintf(out, "  linker = %s\n", db_str(&ctx->command));
    fprintf(out, "  ldflags =%s%s%s%s%s\n",
            db_str(&ctx->ldflags),
            db_str(&ctx->lib_dirs),
            db_str(&ctx->lib_names),
            db_str(&ctx->lib_files),
            db_str(&ctx->frameworks));
    if( ctx->deffile.len > 0 )
        fprintf(out, "  deffile = %s\n", db_str(&ctx->deffile));
    if( ismsvc )
        fprintf(out, "  ismsvc = 1\n");
    if( ismac )
        fprintf(out, "  ismac = 1\n");
}

static void write_link_lib(NinjaCtx* ctx)
{
    /* emits a static-library archive build statement. */
    FILE* out = ctx->out;

    fprintf(out, "build %s: ar %s\n",
            db_str(&ctx->outfile),
            db_str(&ctx->infiles));
    fprintf(out, "  arcmd = %s\n", db_str(&ctx->command));
}

/* When a Qt tool is built by this project (e.g. LeanQt builds and copies its own moc/rcc),
   its executable is produced by a cp build statement emitted earlier as a dependency. The tool-running
   build statement must wait for that copy, so we add the tool as an order-only dependency */

static void append_tool_orderdep(NinjaCtx* ctx, DynBuf* dep)
{
    DynBuf esc;
    db_init(&esc);
    db_append_escaped(&esc, db_str(&ctx->command));
    db_clear(dep);
    if( *db_str(&esc) && strset_contains(&ctx->emitted, db_str(&esc)) )
    {
        db_append(dep, " || ");
        db_append(dep, db_str(&esc));
    }
    db_free(&esc);
}

static void write_moc(NinjaCtx* ctx)
{
    FILE* out = ctx->out;
    DynBuf dep;
    db_init(&dep);
    append_tool_orderdep(ctx,&dep);

    fprintf(out, "build %s: moc %s%s\n",
            db_str(&ctx->outfile),
            db_str(&ctx->infiles),
            db_str(&dep));
    fprintf(out, "  moccmd = %s\n", db_str(&ctx->command));
    if( ctx->defines.len > 0 )
        fprintf(out, "  mocdefs =%s\n", db_str(&ctx->defines));
    if( ctx->mocargs.len > 0 )
        fprintf(out, "  mocargs =%s\n", db_str(&ctx->mocargs));
    db_free(&dep);
}

static void write_rcc(NinjaCtx* ctx)
{
    FILE* out = ctx->out;
    DynBuf dep;
    db_init(&dep);
    append_tool_orderdep(ctx,&dep);

    fprintf(out, "build %s: rcc %s%s\n",
            db_str(&ctx->outfile),
            db_str(&ctx->infiles),
            db_str(&dep));
    fprintf(out, "  rcccmd = %s\n", db_str(&ctx->command));
    fprintf(out, "  rccname = %s\n", db_str(&ctx->name));
    db_free(&dep);
}

static void write_uic(NinjaCtx* ctx)
{
    FILE* out = ctx->out;
    DynBuf dep;
    db_init(&dep);
    append_tool_orderdep(ctx,&dep);

    fprintf(out, "build %s: uic %s%s\n",
            db_str(&ctx->outfile),
            db_str(&ctx->infiles),
            db_str(&dep));
    fprintf(out, "  uiccmd = %s\n", db_str(&ctx->command));
    db_free(&dep);
}

static void write_copy(NinjaCtx* ctx)
{
    FILE* out = ctx->out;

    fprintf(out, "build %s: cp %s\n",
            db_str(&ctx->outfile),
            db_str(&ctx->infiles));
}

static void write_lua(NinjaCtx* ctx)
{
    FILE* out = ctx->out;

    /* dependency products become implicit deps (rebuild on change, but not
       part of $in so the script path stays addressable via $luascript) */
    DynBuf deps;
    db_init(&deps);
    if( ctx->depfiles.len > 0 )
    {
        db_append(&deps, " | ");
        db_append(&deps, db_str(&ctx->depfiles));
    }

    if( ctx->outfile.len > 0 )
    {
        fprintf(out, "build %s: run_lua %s%s\n",
                db_str(&ctx->outfile),
                db_str(&ctx->infiles),
                db_str(&deps));
    }else
    {
        /* a script without declared outputs produces its stamp file instead */
        fprintf(out, "build %s.stamp: run_lua_stamp %s%s\n",
                db_str(&ctx->infiles),
                db_str(&ctx->infiles),
                db_str(&deps));
    }
    db_free(&deps);
    fprintf(out, "  luacmd = %s\n", db_str(&ctx->command));
    fprintf(out, "  luascript = %s\n", db_str(&ctx->infiles));
    if( ctx->args.len > 0 )
        fprintf(out, "  luaargs =%s\n", db_str(&ctx->args));
}

static void ninja_end(void* data)
{
    NinjaCtx* ctx = (NinjaCtx*)data;

    /* deduplicate: skip a build statement whose output was already emitted.
       the build target is the outfile, except for a stampless lua script which targets its infile. */
    const char* key = ctx->outfile.len > 0 ? db_str(&ctx->outfile) : db_str(&ctx->infiles);
    if( *key && strset_seen(&ctx->emitted, key) )
        return;

    switch(ctx->op)
    {
    case BS_Compile:
        write_compile(ctx);
        break;
    case BS_LinkExe:
        write_link_exe(ctx);
        break;
    case BS_LinkDll:
        write_link_dll(ctx);
        break;
    case BS_LinkLib:
        write_link_lib(ctx);
        break;
    case BS_RunMoc:
        write_moc(ctx);
        break;
    case BS_RunRcc:
        write_rcc(ctx);
        break;
    case BS_RunUic:
        write_uic(ctx);
        break;
    case BS_Copy:
        write_copy(ctx);
        break;
    case BS_RunLua:
        write_lua(ctx);
        break;
    default:
        break;
    }
}

static void write_rules(FILE* out, int toolchain, int os)
{
    const int ismsvc = (toolchain == BS_msvc);

    fprintf(out, "# generated by BUSY, do not modify\n");
    fprintf(out, "ninja_required_version = 1.3\n\n");

    /* GCC/Clang compile rule with depfile tracking */
    fprintf(out, "rule cc_gcc\n");
    fprintf(out, "  depfile = $out.d\n");
    fprintf(out, "  deps = gcc\n");
    fprintf(out, "  command = $cc -c $cflags -o \"$out\" \"$in\" -MMD -MF \"$out.d\"\n");
    fprintf(out, "  description = CC $out\n\n");

    /* MSVC compile rule with /showIncludes tracking */
    fprintf(out, "rule cc_msvc\n");
    fprintf(out, "  deps = msvc\n");
    fprintf(out, "  command = $cc /nologo /showIncludes /c $cflags /Fo\"$out\" \"$in\"\n");
    fprintf(out, "  description = CC $out\n\n");

    /* link executable */
    if( ismsvc )
    {
        fprintf(out, "rule link_exe\n");
        fprintf(out, "  command = $linker /nologo $ldflags /out:\"$out\" $in\n");
        fprintf(out, "  description = LINK $out\n\n");
    }else
    {
        fprintf(out, "rule link_exe\n");
        fprintf(out, "  command = $linker $in -o \"$out\" $ldflags\n");
        fprintf(out, "  description = LINK $out\n\n");
    }

    /* link shared library */
    if( ismsvc )
    {
        fprintf(out, "rule link_dll\n");
        fprintf(out, "  command = $linker /nologo /dll $ldflags $defflag /out:\"$out\" /implib:\"$out.lib\" $in\n");
        fprintf(out, "  description = LINK $out\n\n");
    }else if( os == BS_mac )
    {
        fprintf(out, "rule link_dll\n");
        fprintf(out, "  command = $linker -dynamiclib $in -o \"$out\" $ldflags\n");
        fprintf(out, "  description = LINK $out\n\n");
    }else
    {
        fprintf(out, "rule link_dll\n");
        fprintf(out, "  command = $linker -shared $in -o \"$out\" $ldflags\n");
        fprintf(out, "  description = LINK $out\n\n");
    }

    /* static library archive */
    if( ismsvc )
    {
        fprintf(out, "rule ar\n");
        fprintf(out, "  command = $arcmd /nologo /out:\"$out\" $in\n");
        fprintf(out, "  description = AR $out\n\n");
    }else
    {
        fprintf(out, "rule ar\n");
        fprintf(out, "  command = $arcmd r \"$out\" $in\n");
        fprintf(out, "  description = AR $out\n\n");
    }

    /* moc */
    fprintf(out, "rule moc\n");
    fprintf(out, "  command = $moccmd $mocdefs \"$in\" -o \"$out\" $mocargs\n");
    fprintf(out, "  description = MOC $out\n\n");

    /* rcc */
    fprintf(out, "rule rcc\n");
    fprintf(out, "  command = $rcccmd \"$in\" -o \"$out\" -name \"$rccname\"\n");
    fprintf(out, "  description = RCC $out\n\n");

    /* uic */
    fprintf(out, "rule uic\n");
    fprintf(out, "  command = $uiccmd \"$in\" -o \"$out\"\n");
    fprintf(out, "  description = UIC $out\n\n");

    /* copy */
#ifdef _WIN32
    fprintf(out, "rule cp\n");
    fprintf(out, "  command = copy /Y \"$in\" \"$out\"\n");
    fprintf(out, "  description = COPY $out\n\n");
#else
    fprintf(out, "rule cp\n");
    fprintf(out, "  command = cp \"$in\" \"$out\"\n");
    fprintf(out, "  description = COPY $out\n\n");
#endif

    /* lua script */
    fprintf(out, "rule run_lua\n");
    fprintf(out, "  command = $luacmd \"$luascript\" $luaargs\n");
    fprintf(out, "  description = LUA $out\n\n");
    fprintf(out, "rule run_lua_stamp\n");
    fprintf(out, "  command = $luacmd \"$luascript\" $luaargs && touch \"$out\"\n");
    fprintf(out, "  description = LUA $out\n\n");
}

int bs_genNinja(lua_State* L)
{
    /* main entry point: args are root module def, array of productinst */
    enum { ROOT = 1, PRODS };
    const int top = lua_gettop(L);
    size_t i;

    lua_getglobal(L, "require");
    lua_pushstring(L, "builtins");
    lua_call(L,1,1);
    const int builtins = lua_gettop(L);

    lua_getfield(L,builtins,"#inst");
    const int binst = lua_gettop(L);

    lua_getfield(L,binst,"root_build_dir");
    const int buildDir = lua_gettop(L);

    if( !bs_exists(lua_tostring(L,buildDir)) )
    {
        if( bs_mkdir(lua_tostring(L,buildDir)) != 0 )
            luaL_error(L,"error creating directory %s", lua_tostring(L,buildDir));
    }

    /* determine target toolchain and OS for rule generation */
    const BSToolchain toolchain = bs_getToolchain(L,binst,0);
    const BSOperatingSystem os = bs_getOperatingSystem(L,binst,0);

    lua_pushvalue(L,buildDir);
    lua_pushstring(L,"/build.ninja");
    lua_concat(L,2);
    const int ninjaPath = lua_gettop(L);

    fprintf(stdout,"# generating %s\n", bs_denormalize_path(lua_tostring(L,ninjaPath)));
    fflush(stdout);

    FILE* out = bs_fopen(bs_denormalize_path(lua_tostring(L,ninjaPath)),"w");
    if( out == NULL )
        luaL_error(L,"cannot open file for writing: %s", lua_tostring(L,ninjaPath));

    write_rules(out, toolchain, os);

    NinjaCtx nctx;
    ctx_init(&nctx);
    nctx.out = out;

    for( i = 1; i <= lua_objlen(L,PRODS); i++ )
    {
        lua_pushcfunction(L, bs_visit);
        lua_rawgeti(L,PRODS,i);
        BSVisitorCtx* vctx = bs_newctx(L);
        vctx->d_data = &nctx;
        vctx->d_log = 0;
        vctx->d_loggerData = 0;
        vctx->d_begin = ninja_begin;
        vctx->d_param = ninja_param;
        vctx->d_end = ninja_end;
        vctx->d_fork = 0;
        lua_call(L,2,0);
    }

    fclose(out);

    ctx_free(&nctx);

    /* reset #out so a subsequent direct build would still work */
    lua_pushcfunction(L, bs_resetOut);
    lua_pushvalue(L,ROOT);
    lua_call(L,1,0);

    lua_pop(L,4); /* builtins, binst, buildDir, ninjaPath */
    assert( top == lua_gettop(L) );
    return 0;
}
