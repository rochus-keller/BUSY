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

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "bsexec.h"
#include "lauxlib.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/utime.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <utime.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct BSNameList {
    char** names;
    size_t count;
    size_t capacity;
} BSNameList;

typedef struct BSSystemCommand {
    char** argv;
    size_t argc;
    char* storage;
    const char* in;
    const char* out;
    const char* err;
} BSSystemCommand;

static int bs_is_nul_name(const char* path)
{
    return path != 0 &&
        (path[0] == 'n' || path[0] == 'N') &&
        (path[1] == 'u' || path[1] == 'U') &&
        (path[2] == 'l' || path[2] == 'L') &&
        path[3] == 0;
}

static const char* bs_redirect_path(const char* path)
{
    if( path && (strcmp(path, "/dev/null") == 0 || bs_is_nul_name(path)) )
    {
#ifdef _WIN32
        return "NUL";
#else
        return "/dev/null";
#endif
    }
    return path;
}

static int compare_names(const void* a, const void* b)
{
    const char* const* lhs = (const char* const*)a;
    const char* const* rhs = (const char* const*)b;
    return strcmp(*lhs, *rhs);
}

static int add_name(BSNameList* list, const char* name)
{
    char** names;
    char* copy;
    size_t len;
    size_t cap;
    if( list->count == list->capacity )
    {
        cap = list->capacity ? list->capacity * 2 : 16;
        if( cap < list->capacity || cap > ((size_t)-1) / sizeof(char*) )
        {
            errno = ENOMEM;
            return -1;
        }
        names = (char**)realloc(list->names, cap * sizeof(char*));
        if( names == 0 )
            return -1;
        list->names = names;
        list->capacity = cap;
    }
    len = strlen(name) + 1;
    copy = (char*)malloc(len);
    if( copy == 0 )
        return -1;
    memcpy(copy, name, len);
    list->names[list->count++] = copy;
    return 0;
}

static void free_names(BSNameList* list)
{
    size_t i;
    for( i = 0; i < list->count; i++ )
        free(list->names[i]);
    free(list->names);
}

int bs_os_getcwd(char* path, int len)
{
#ifdef _WIN32
    return _getcwd(path, len) ? 0 : -1;
#else
    return getcwd(path, (size_t)len) ? 0 : -1;
#endif
}

int bs_os_app_path(char* path, int len)
{
#ifdef _WIN32
    return (int)GetModuleFileNameA(0, path, (DWORD)len);
#else
    ssize_t res = readlink("/proc/self/exe", path, (size_t)len - 1);
    if( res < 0 )
    {
        res = readlink("/proc/curproc/file", path, (size_t)len - 1);
        if( res < 0 )
        {
            res = readlink("/proc/self/path/a.out", path, (size_t)len - 1);
            if( res < 0 )
                return -1;
        }
    }
    path[res] = 0;
    return (int)res;
#endif
}

int bs_os_mtime(const char* path, time_t* result)
{
    struct stat st;
    if( stat(path, &st) != 0 )
        return -1;
    *result = st.st_mtime;
    return 0;
}

int bs_os_touch(const char* path)
{
#ifdef _WIN32
    return _utime(path, 0);
#else
    return utime(path, 0);
#endif
}

int bs_os_path_max(void)
{
#ifdef _WIN32
    return MAX_PATH;
#else
    return PATH_MAX;
#endif
}

int bs_mkdir2(const char* path)
{
#ifdef _WIN32
    int res = _mkdir(path);
#else
    int res = mkdir(path, 0777);
#endif
    if( res == -1 && errno == EEXIST )
        return 0;
    else
        return res;
}

int bs_listdir(const char* dir, void (*cb)(const char*, void*), void* data)
{
    BSNameList list;
    size_t i;
    list.names = 0;
    list.count = 0;
    list.capacity = 0;
#ifdef _WIN32
    {
        WIN32_FIND_DATAA found;
        HANDLE h;
        DWORD attrs;
        size_t len;
        char* pattern;
        attrs = GetFileAttributesA(dir);
        if( attrs == INVALID_FILE_ATTRIBUTES ||
            (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 )
        {
            errno = ENOENT;
            return -1;
        }
        len = strlen(dir);
        pattern = (char*)malloc(len + 4);
        if( pattern == 0 )
            return -1;
        memcpy(pattern, dir, len);
        if( len && dir[len-1] != '/' && dir[len-1] != '\\' )
            pattern[len++] = '\\';
        pattern[len++] = '*';
        pattern[len] = 0;
        h = FindFirstFileA(pattern, &found);
        free(pattern);
        if( h == INVALID_HANDLE_VALUE )
        {
            if( GetLastError() == ERROR_FILE_NOT_FOUND )
                return 0;
            errno = EIO;
            return -1;
        }
        do {
            if( strcmp(found.cFileName, ".") != 0 &&
                strcmp(found.cFileName, "..") != 0 &&
                add_name(&list, found.cFileName) != 0 )
            {
                FindClose(h);
                free_names(&list);
                return -1;
            }
        } while( FindNextFileA(h, &found) );
        FindClose(h);
    }
#else
    {
        DIR* d;
        struct dirent* ent;
        d = opendir(dir);
        if( d == 0 )
            return -1;
        errno = 0;
        while( (ent = readdir(d)) != 0 )
        {
            if( strcmp(ent->d_name, ".") != 0 &&
                strcmp(ent->d_name, "..") != 0 &&
                add_name(&list, ent->d_name) != 0 )
            {
                int e = errno;
                closedir(d);
                free_names(&list);
                errno = e;
                return -1;
            }
        }
        if( errno != 0 )
        {
            int e = errno;
            closedir(d);
            free_names(&list);
            errno = e;
            return -1;
        }
        closedir(d);
    }
#endif
    if( list.count > 1 )
        qsort(list.names, list.count, sizeof(char*), compare_names);
    for( i = 0; i < list.count; i++ )
        cb(list.names[i], data);
    free_names(&list);
    return 0;
}

int bs_exists3(const char* path)
{
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    if( attrs == INVALID_FILE_ATTRIBUTES )
        return 0;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
#else
    struct stat st;
    if( stat(path, &st) != 0 )
        return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
#endif
}

int bs_mkdirp(const char* path)
{
    char* buf;
    size_t len;
    char* p;
    int result = 0;
    if( path == 0 || *path == 0 )
    {
        errno = EINVAL;
        return -1;
    }
    len = strlen(path);
    buf = (char*)malloc(len + 1);
    if( buf == 0 )
        return -1;
    memcpy(buf, path, len + 1);
#ifdef _WIN32
    p = buf + 1;
    if( len > 1 && ((buf[0] == '\\' && buf[1] == '\\') ||
                    (buf[0] == '/' && buf[1] == '/')) )
    {
        p = buf + 2;
        while( *p && *p != '/' && *p != '\\' ) p++;
        if( *p ) p++;
        while( *p && *p != '/' && *p != '\\' ) p++;
        if( *p ) p++;
    }
    for( ; *p; p++ )
    {
        if( *p == '/' || *p == '\\' )
        {
            char saved = *p;
            *p = 0;
            if( strlen(buf) > 2 && CreateDirectoryA(buf, 0) == 0 )
            {
                DWORD error = GetLastError();
                DWORD attrs = GetFileAttributesA(buf);
                if( error != ERROR_ALREADY_EXISTS ||
                    attrs == INVALID_FILE_ATTRIBUTES ||
                    (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 )
                {
                    errno = EIO;
                    result = -1;
                    *p = saved;
                    break;
                }
            }
            *p = saved;
        }
    }
    if( result == 0 && CreateDirectoryA(buf, 0) == 0 )
    {
        DWORD error = GetLastError();
        DWORD attrs = GetFileAttributesA(buf);
        if( error != ERROR_ALREADY_EXISTS ||
            attrs == INVALID_FILE_ATTRIBUTES ||
            (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 )
        {
            errno = EIO;
            result = -1;
        }
    }
#else
    p = buf + (buf[0] == '/' ? 1 : 0);
    for( ; *p; p++ )
    {
        if( *p == '/' )
        {
            char saved = *p;
            *p = 0;
            if( *buf && mkdir(buf, 0777) != 0 && errno != EEXIST )
            {
                result = -1;
                *p = saved;
                break;
            }
            if( *buf && errno == EEXIST )
            {
                struct stat st;
                if( stat(buf, &st) != 0 || !S_ISDIR(st.st_mode) )
                {
                    errno = ENOTDIR;
                    result = -1;
                    *p = saved;
                    break;
                }
            }
            *p = saved;
        }
    }
    if( result == 0 && mkdir(buf, 0777) != 0 && errno != EEXIST )
        result = -1;
    if( result == 0 )
    {
        struct stat st;
        if( stat(buf, &st) != 0 || !S_ISDIR(st.st_mode) )
        {
            errno = ENOTDIR;
            result = -1;
        }
    }
#endif
    free(buf);
    return result;
}

int bs_copyfile(const char* to, const char* from)
{
    FILE* src;
    FILE* dst;
    char buffer[8192];
    size_t n;
    int failed = 0;
    int saved = 0;
#ifndef _WIN32
    struct stat source_stat;
    mode_t source_mode;
    mode_t mask;
#endif
    src = fopen(from, "rb");
    if( src == 0 )
        return -1;
#ifndef _WIN32
    if( fstat(fileno(src), &source_stat) != 0 )
    {
        saved = errno;
        fclose(src);
        errno = saved;
        return -1;
    }
    mask = umask(0);
    umask(mask);
    source_mode = source_stat.st_mode & 0777 & ~mask;
#endif
    dst = fopen(to, "wb");
    if( dst == 0 )
    {
        saved = errno;
        fclose(src);
        errno = saved;
        return -1;
    }
    while( (n = fread(buffer, 1, sizeof(buffer), src)) != 0 )
    {
        if( fwrite(buffer, 1, n, dst) != n )
        {
            failed = 1;
            saved = errno ? errno : EIO;
            break;
        }
    }
    if( ferror(src) )
    {
        failed = 1;
        saved = errno ? errno : EIO;
    }
    if( fclose(src) != 0 && !failed )
    {
        failed = 1;
        saved = errno ? errno : EIO;
    }
    if( fclose(dst) != 0 && !failed )
    {
        failed = 1;
        saved = errno ? errno : EIO;
    }
    if( failed )
    {
        remove(to);
        errno = saved;
        return -1;
    }
#ifndef _WIN32
    if( chmod(to, source_mode) != 0 )
    {
        saved = errno;
        remove(to);
        errno = saved;
        return -1;
    }
#endif
    return 0;
}

typedef struct BSCopyTreeError {
    char* from;
    char* to;
} BSCopyTreeError;

typedef struct BSCopyTreeList {
    const char* from;
    const char* to;
    int result;
    BSCopyTreeError* error;
} BSCopyTreeList;

static char* copytree_join(const char* dir, const char* name)
{
    size_t dirlen = strlen(dir);
    size_t namelen = strlen(name);
    int slash = dirlen != 0 && dir[dirlen - 1] != '/' &&
                dir[dirlen - 1] != '\\';
    char* path;
    if( dirlen > (size_t)-1 - namelen - (size_t)slash - 1 )
    {
        errno = ENOMEM;
        return 0;
    }
    path = (char*)malloc(dirlen + namelen + (size_t)slash + 1);
    if( path == 0 )
        return 0;
    memcpy(path, dir, dirlen);
    if( slash )
        path[dirlen++] = '/';
    memcpy(path + dirlen, name, namelen + 1);
    return path;
}

static char* copytree_dup(const char* text)
{
    size_t len = strlen(text) + 1;
    char* copy = (char*)malloc(len);
    if( copy )
        memcpy(copy, text, len);
    return copy;
}

static void copytree_error(BSCopyTreeError* error, const char* from,
                           const char* to)
{
    if( error && error->from == 0 )
    {
        error->from = copytree_dup(from);
        error->to = copytree_dup(to);
    }
}

static int copytree_mkdir_parent(const char* path)
{
    char* parent = copytree_dup(path);
    char* p;
    int result = 0;
    if( parent == 0 )
        return -1;
    p = strrchr(parent, '/');
    if( p == 0 )
        p = strrchr(parent, '\\');
    if( p )
    {
        if( p == parent )
            p[1] = 0;
        else
            *p = 0;
        if( *parent )
            result = bs_mkdirp(parent);
    }
    free(parent);
    return result;
}

static int copytree_impl(const char* to, const char* from,
                         BSCopyTreeError* error);

static void copytree_entry(const char* name, void* data)
{
    BSCopyTreeList* list = (BSCopyTreeList*)data;
    char* from;
    char* to;
    if( list->result != 0 )
        return;
    from = copytree_join(list->from, name);
    to = copytree_join(list->to, name);
    if( from == 0 || to == 0 )
    {
        int e = errno ? errno : ENOMEM;
        free(from);
        free(to);
        copytree_error(list->error, list->from, list->to);
        errno = e;
        list->result = -1;
        return;
    }
    if( copytree_impl(to, from, list->error) != 0 )
        list->result = -1;
    free(from);
    free(to);
}

static int copytree_impl(const char* to, const char* from,
                         BSCopyTreeError* error)
{
    int kind;
    int result;
    errno = 0;
    kind = bs_exists3(from);
    if( kind == 0 )
    {
        if( errno == 0 )
            errno = ENOENT;
        copytree_error(error, from, to);
        return -1;
    }
    if( kind == 1 )
    {
        if( copytree_mkdir_parent(to) != 0 || bs_copyfile(to, from) != 0 )
        {
            int e = errno;
            copytree_error(error, from, to);
            errno = e;
            return -1;
        }
        return 0;
    }
    if( bs_mkdirp(to) != 0 )
    {
        int e = errno;
        copytree_error(error, from, to);
        errno = e;
        return -1;
    }
    {
        BSCopyTreeList list;
        list.from = from;
        list.to = to;
        list.result = 0;
        list.error = error;
        result = bs_listdir(from, copytree_entry, &list);
        if( result != 0 || list.result != 0 )
        {
            int e = errno;
            copytree_error(error, from, to);
            errno = e;
            return -1;
        }
    }
    return 0;
}

int bs_copytree(const char* to, const char* from)
{
    return copytree_impl(to, from, 0);
}

#ifdef _WIN32
static int append_slashes(char** p, size_t n)
{
    while( n-- )
        *(*p)++ = '\\';
    return 0;
}

static char* make_command_line(const char* const* argv)
{
    const char* const* a;
    size_t size = 1;
    char* line;
    char* p;
    int first = 1;
    for( a = argv; *a; a++ )
    {
        size_t n = strlen(*a);
        if( n > (((size_t)-1) - size) / 2 - 4 )
        {
            errno = ENOMEM;
            return 0;
        }
        size += n * 2 + 4;
    }
    line = (char*)malloc(size);
    if( line == 0 )
        return 0;
    p = line;
    for( a = argv; *a; a++ )
    {
        const char* s = *a;
        int quote = (*s == 0 || strpbrk(s, " \t\"") != 0);
        size_t slashes = 0;
        if( !first )
            *p++ = ' ';
        first = 0;
        if( quote )
            *p++ = '"';
        while( *s )
        {
            if( *s == '\\' )
                slashes++;
            else if( *s == '"' )
            {
                append_slashes(&p, slashes * 2 + 1);
                *p++ = *s;
                slashes = 0;
            }else
            {
                append_slashes(&p, slashes);
                slashes = 0;
                *p++ = *s;
            }
            s++;
        }
        if( quote )
        {
            append_slashes(&p, slashes * 2);
            *p++ = '"';
        }else
            append_slashes(&p, slashes);
    }
    *p = 0;
    return line;
}

static int win_error(DWORD error)
{
    if( error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND )
        errno = ENOENT;
    else if( error == ERROR_ACCESS_DENIED )
        errno = EACCES;
    else
        errno = EIO;
    return -1;
}

static HANDLE open_win_file(const char* path, int input)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = 0;
    sa.bInheritHandle = TRUE;
    return CreateFileA(path, input ? GENERIC_READ : GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       input ? OPEN_EXISTING : CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, 0);
}

int bs_spawn(const char* const* argv, const char* cwd, const char* in,
             const char* out, const char* err)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE hin, hout, herr;
    char* command;
    DWORD code;
    const char* in_path;
    const char* out_path;
    const char* err_path;
    hin = INVALID_HANDLE_VALUE;
    hout = INVALID_HANDLE_VALUE;
    herr = INVALID_HANDLE_VALUE;
    in_path = bs_redirect_path(in);
    out_path = bs_redirect_path(out);
    err_path = bs_redirect_path(err);
    if( in_path && (hin = open_win_file(in_path, 1)) == INVALID_HANDLE_VALUE )
        return win_error(GetLastError());
    if( out_path && (hout = open_win_file(out_path, 0)) == INVALID_HANDLE_VALUE )
    {
        DWORD e = GetLastError();
        if( hin != INVALID_HANDLE_VALUE ) CloseHandle(hin);
        return win_error(e);
    }
    if( err_path && out_path && strcmp(err_path, out_path) == 0 )
        herr = hout;
    else if( err_path && (herr = open_win_file(err_path, 0)) == INVALID_HANDLE_VALUE )
    {
        DWORD e = GetLastError();
        if( hin != INVALID_HANDLE_VALUE ) CloseHandle(hin);
        if( hout != INVALID_HANDLE_VALUE ) CloseHandle(hout);
        return win_error(e);
    }
    command = make_command_line(argv);
    if( command == 0 )
    {
        if( hin != INVALID_HANDLE_VALUE ) CloseHandle(hin);
        if( hout != INVALID_HANDLE_VALUE ) CloseHandle(hout);
        if( herr != INVALID_HANDLE_VALUE && herr != hout ) CloseHandle(herr);
        return -1;
    }
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hin == INVALID_HANDLE_VALUE ? GetStdHandle(STD_INPUT_HANDLE) : hin;
    si.hStdOutput = hout == INVALID_HANDLE_VALUE ? GetStdHandle(STD_OUTPUT_HANDLE) : hout;
    si.hStdError = herr == INVALID_HANDLE_VALUE ? GetStdHandle(STD_ERROR_HANDLE) : herr;
    if( !CreateProcessA(0, command, 0, 0, TRUE, 0, 0, cwd, &si, &pi) )
    {
        DWORD e = GetLastError();
        free(command);
        if( hin != INVALID_HANDLE_VALUE ) CloseHandle(hin);
        if( hout != INVALID_HANDLE_VALUE ) CloseHandle(hout);
        if( herr != INVALID_HANDLE_VALUE && herr != hout ) CloseHandle(herr);
        return win_error(e);
    }
    free(command);
    if( hin != INVALID_HANDLE_VALUE ) CloseHandle(hin);
    if( hout != INVALID_HANDLE_VALUE ) CloseHandle(hout);
    if( herr != INVALID_HANDLE_VALUE && herr != hout ) CloseHandle(herr);
    if( WaitForSingleObject(pi.hProcess, INFINITE) != WAIT_OBJECT_0 )
    {
        DWORD e = GetLastError();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return win_error(e);
    }
    if( !GetExitCodeProcess(pi.hProcess, &code) )
    {
        DWORD e = GetLastError();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return win_error(e);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

#else
static void child_error(int fd, int error)
{
    const char* p = (const char*)&error;
    size_t left = sizeof(error);
    while( left )
    {
        ssize_t n = write(fd, p, left);
        if( n > 0 )
        {
            p += n;
            left -= (size_t)n;
        }else if( n < 0 && errno == EINTR )
            continue;
        else
            break;
    }
}

int bs_spawn(const char* const* argv, const char* cwd, const char* in,
             const char* out, const char* err)
{
    int pipefd[2];
    int infd = -1;
    int outfd = -1;
    int errfd = -1;
    int childerr = 0;
    size_t got = 0;
    pid_t pid;
    pid_t waited;
    int status;
    const char* in_path;
    const char* out_path;
    const char* err_path;
    in_path = bs_redirect_path(in);
    out_path = bs_redirect_path(out);
    err_path = bs_redirect_path(err);
    if( in_path && (infd = open(in_path, O_RDONLY)) < 0 )
        return -1;
    if( out_path && (outfd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0 )
    {
        if( infd >= 0 ) close(infd);
        return -1;
    }
    if( err_path && out_path && strcmp(err_path, out_path) == 0 )
        errfd = outfd;
    else if( err_path && (errfd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0 )
    {
        if( infd >= 0 ) close(infd);
        if( outfd >= 0 ) close(outfd);
        return -1;
    }
    if( pipe(pipefd) != 0 )
    {
        int e = errno;
        if( infd >= 0 ) close(infd);
        if( outfd >= 0 ) close(outfd);
        if( errfd >= 0 && errfd != outfd ) close(errfd);
        errno = e;
        return -1;
    }
    if( fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0 )
    {
        int e = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        if( infd >= 0 ) close(infd);
        if( outfd >= 0 ) close(outfd);
        if( errfd >= 0 && errfd != outfd ) close(errfd);
        errno = e;
        return -1;
    }
    fflush(0);
    pid = fork();
    if( pid < 0 )
    {
        int e = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        if( infd >= 0 ) close(infd);
        if( outfd >= 0 ) close(outfd);
        if( errfd >= 0 && errfd != outfd ) close(errfd);
        errno = e;
        return -1;
    }
    if( pid == 0 )
    {
        int e;
        close(pipefd[0]);
        if( cwd && chdir(cwd) != 0 )
        {
            e = errno;
            child_error(pipefd[1], e);
            _exit(127);
        }
        if( (infd >= 0 && dup2(infd, 0) < 0) ||
            (outfd >= 0 && dup2(outfd, 1) < 0) ||
            (errfd >= 0 && dup2(errfd, 2) < 0) )
        {
            e = errno;
            child_error(pipefd[1], e);
            _exit(127);
        }
        if( infd > 2 ) close(infd);
        if( outfd > 2 ) close(outfd);
        if( errfd > 2 && errfd != outfd ) close(errfd);
        execvp(argv[0], (char* const*)argv);
        e = errno;
        child_error(pipefd[1], e);
        _exit(127);
    }
    close(pipefd[1]);
    if( infd >= 0 ) close(infd);
    if( outfd >= 0 ) close(outfd);
    if( errfd >= 0 && errfd != outfd ) close(errfd);
    while( got < sizeof(childerr) )
    {
        ssize_t n = read(pipefd[0], (char*)&childerr + got,
                         sizeof(childerr) - got);
        if( n > 0 )
            got += (size_t)n;
        else if( n == 0 )
            break;
        else if( errno != EINTR )
        {
            int e = errno;
            close(pipefd[0]);
            while( waitpid(pid, &status, 0) < 0 && errno == EINTR ) {}
            errno = e;
            return -1;
        }
    }
    close(pipefd[0]);
    do {
        waited = waitpid(pid, &status, 0);
    } while( waited < 0 && errno == EINTR );
    if( waited < 0 )
        return -1;
    if( got != 0 )
    {
        errno = childerr;
        return -1;
    }
    if( WIFEXITED(status) )
        return WEXITSTATUS(status);
    if( WIFSIGNALED(status) )
        return 128 + WTERMSIG(status);
    return 255;
}
#endif

static int bs_system_meta(int ch)
{
    return ch == '|' || ch == '&' || ch == ';' || ch == '<' ||
           ch == '>' || ch == '`' || ch == '$' || ch == '(' || ch == ')';
}

static int bs_system_error(const char* message)
{
    fprintf(stderr, "bs_system: %s\n", message);
    errno = EINVAL;
    return -1;
}

static int bs_parse_word(const char** input, char** output, char** value)
{
    const char* p;
    char* q;
    char* start;
    int started;
    int closed;
#ifdef _WIN32
    int n;
    int i;
#endif
    p = *input;
    q = *output;
    start = q;
    started = 0;
    while( *p && !isspace((unsigned char)*p) )
    {
        if( *p == '"' )
        {
            started = 1;
            p++;
            closed = 0;
            while( *p )
            {
                if( *p == '"' )
                {
                    p++;
                    closed = 1;
                    break;
                }
#ifdef _WIN32
                if( *p == '\\' )
                {
                    /* MS CRT rule: backslashes are literal unless they precede a quote */
                    n = 0;
                    while( p[n] == '\\' )
                        n++;
                    if( p[n] == '"' )
                    {
                        for( i = 0; i < n / 2; i++ )
                            *q++ = '\\';
                        p += n;
                        if( n % 2 )
                            *q++ = *p++;
                    }else
                    {
                        for( i = 0; i < n; i++ )
                            *q++ = *p++;
                    }
                }else
#else
                if( *p == '\\' && (p[1] == '"' || p[1] == '\\') )
                {
                    *q++ = p[1];
                    p += 2;
                }else
#endif
                {
                    *q++ = *p++;
                }
            }
            if( !closed )
                return bs_system_error("unterminated double quote");
        }else if( *p == '\'' )
        {
            started = 1;
            p++;
            closed = 0;
            while( *p )
            {
                if( *p == '\'' )
                {
                    p++;
                    closed = 1;
                    break;
                }
                *q++ = *p++;
            }
            if( !closed )
                return bs_system_error("unterminated single quote");
        }else if( bs_system_meta((unsigned char)*p) )
        {
            fprintf(stderr, "bs_system: unsupported unquoted metacharacter '%c'\n",
                    *p);
            errno = EINVAL;
            return -1;
        }else
        {
            started = 1;
            *q++ = *p++;
        }
    }
    if( !started )
        return bs_system_error("empty command word");
    *q++ = 0;
    *input = p;
    *output = q;
    *value = start;
    return 0;
}

static int bs_redirection_start(const char* input, int* stream, int* length)
{
    if( input[0] == '<' )
    {
        *stream = 1;
        *length = 1;
        return 1;
    }
    if( input[0] == '>' )
    {
        *stream = 2;
        *length = 1;
        return 1;
    }
    if( input[0] == '2' && input[1] == '>' )
    {
        *stream = 3;
        *length = 2;
        return 1;
    }
    return 0;
}

static int bs_parse_command(const char* input, BSSystemCommand* command)
{
    const char* p;
    char* q;
    char* word;
    size_t length;
    size_t capacity;
    int stream;
    int op_length;
    p = input;
    length = strlen(input);
    if( length > (size_t)-1 - 2 ||
        length + 2 > (size_t)-1 / sizeof(char*) )
        return bs_system_error("command is too long");
    command->storage = (char*)malloc(length + 1);
    command->argv = (char**)malloc((length + 2) * sizeof(char*));
    command->argc = 0;
    command->in = 0;
    command->out = 0;
    command->err = 0;
    if( command->storage == 0 || command->argv == 0 )
    {
        free(command->storage);
        free(command->argv);
        command->storage = 0;
        command->argv = 0;
        return bs_system_error("out of memory");
    }
    capacity = length + 2;
    q = command->storage;
    while( *p )
    {
        while( *p && isspace((unsigned char)*p) )
            p++;
        if( !*p )
            break;
        if( bs_redirection_start(p, &stream, &op_length) )
        {
            p += op_length;
            while( *p && isspace((unsigned char)*p) )
                p++;
            if( !*p )
            {
                bs_system_error("redirection has no target");
                goto fail;
            }
            if( bs_parse_word(&p, &q, &word) != 0 )
                goto fail;
            if( *word == 0 )
            {
                bs_system_error("redirection has an empty target");
                goto fail;
            }
            if( (stream == 1 && command->in) ||
                (stream == 2 && command->out) ||
                (stream == 3 && command->err) )
            {
                bs_system_error("repeated redirection is not supported");
                goto fail;
            }
            if( stream == 1 )
                command->in = word;
            else if( stream == 2 )
                command->out = word;
            else
                command->err = word;
        }else
        {
            if( command->argc + 1 >= capacity )
            {
                bs_system_error("too many command words");
                goto fail;
            }
            if( bs_parse_word(&p, &q, &word) != 0 )
                goto fail;
            command->argv[command->argc++] = word;
        }
    }
    if( command->argc == 0 )
    {
        bs_system_error("empty command");
        goto fail;
    }
    command->argv[command->argc] = 0;
    return 0;

fail:
    free(command->storage);
    free(command->argv);
    command->storage = 0;
    command->argv = 0;
    return -1;
}

static void bs_free_system_command(BSSystemCommand* command)
{
    free(command->storage);
    free(command->argv);
    command->storage = 0;
    command->argv = 0;
}

static int bs_system_copy(const char* to, const char* from)
{
    char* parent;
    char* slash;
    char* backslash;
    int result;
    parent = (char*)malloc(strlen(to) + 1);
    if( parent == 0 )
    {
        errno = ENOMEM;
        return -1;
    }
    strcpy(parent, to);
    slash = strrchr(parent, '/');
    backslash = strrchr(parent, '\\');
    if( backslash && (!slash || backslash > slash) )
        slash = backslash;
    if( slash )
    {
        if( slash == parent )
            slash[1] = 0;
        else
            *slash = 0;
        result = bs_mkdirp(parent);
        if( result != 0 )
        {
            free(parent);
            return -1;
        }
    }
    free(parent);
    return bs_copyfile(to, from);
}

int bs_system(const char* cmd)
{
    BSSystemCommand command;
    int result;
    if( cmd == 0 )
        return bs_system_error("null command");
    if( bs_parse_command(cmd, &command) != 0 )
        return -1;
    if( command.argc == 3 && strcmp(command.argv[0], "copy") == 0 )
    {
        if( command.in || command.out || command.err )
        {
            bs_system_error("copy does not support redirection");
            bs_free_system_command(&command);
            return -1;
        }
        result = bs_system_copy(command.argv[2], command.argv[1]);
    }else
        result = bs_spawn((const char* const*)command.argv, 0, command.in,
                          command.out, command.err);
    bs_free_system_command(&command);
    return result;
}

static int l_mkdir(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    if( bs_mkdirp(path) != 0 )
    {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_exists(lua_State* L)
{
    int kind = bs_exists3(luaL_checkstring(L, 1));
    if( kind == 1 )
        lua_pushstring(L, "file");
    else if( kind == 2 )
        lua_pushstring(L, "dir");
    else
        lua_pushnil(L);
    return 1;
}

typedef struct BSLuaNames {
    lua_State* L;
    int index;
} BSLuaNames;

static void push_name(const char* name, void* data)
{
    BSLuaNames* list = (BSLuaNames*)data;
    lua_pushstring(list->L, name);
    lua_rawseti(list->L, -2, ++list->index);
}

static int l_listdir(lua_State* L)
{
    BSLuaNames list;
    luaL_checkstring(L, 1);
    lua_newtable(L);
    list.L = L;
    list.index = 0;
    if( bs_listdir(lua_tostring(L, 1), push_name, &list) != 0 )
    {
        int e = errno;
        lua_settop(L, 0);
        lua_pushnil(L);
        lua_pushstring(L, strerror(e));
        return 2;
    }
    return 1;
}

static int l_copy(lua_State* L)
{
    const char* to = luaL_checkstring(L, 1);
    const char* from = luaL_checkstring(L, 2);
    if( bs_copyfile(to, from) != 0 )
    {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_copytree(lua_State* L)
{
    const char* to = luaL_checkstring(L, 1);
    const char* from = luaL_checkstring(L, 2);
    BSCopyTreeError error;
    int result;
    int e;
    error.from = 0;
    error.to = 0;
    result = copytree_impl(to, from, &error);
    e = errno;
    if( result != 0 )
    {
        lua_pushnil(L);
        if( error.from && error.to )
            lua_pushfstring(L, "%s: %s -> %s", strerror(e), error.from,
                            error.to);
        else
            lua_pushstring(L, strerror(e));
        free(error.from);
        free(error.to);
        return 2;
    }
    free(error.from);
    free(error.to);
    lua_pushboolean(L, 1);
    return 1;
}

static const char* table_option(lua_State* L, int table, const char* name)
{
    const char* value;
    lua_getfield(L, table, name);
    if( lua_isnil(L, -1) )
        value = 0;
    else
    {
        luaL_checktype(L, -1, LUA_TSTRING);
        value = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static int l_spawn(lua_State* L)
{
    size_t n;
    size_t i;
    const char** argv;
    const char* cwd;
    const char* in;
    const char* out;
    const char* err;
    int result;
    int e;
    luaL_checktype(L, 1, LUA_TTABLE);
    n = lua_objlen(L, 1);
    if( n == 0 || n > (size_t)INT_MAX ||
        n > (size_t)((size_t)-1) / sizeof(char*) - 1 )
        return luaL_error(L, "spawn requires a program");
    for( i = 1; i <= n; i++ )
    {
        lua_rawgeti(L, 1, (int)i);
        luaL_checktype(L, -1, LUA_TSTRING);
        luaL_checkstring(L, -1);
        lua_pop(L, 1);
    }
    cwd = table_option(L, 1, "cwd");
    in = table_option(L, 1, "stdin");
    out = table_option(L, 1, "stdout");
    err = table_option(L, 1, "stderr");
    argv = (const char**)malloc((n + 1) * sizeof(char*));
    if( argv == 0 )
    {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }
    for( i = 1; i <= n; i++ )
    {
        lua_rawgeti(L, 1, (int)i);
        argv[i-1] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    argv[n] = 0;
    result = bs_spawn(argv, cwd, in, out, err);
    e = errno;
    free(argv);
    if( result < 0 )
    {
        lua_pushnil(L);
        lua_pushstring(L, strerror(e));
        return 2;
    }
    lua_pushinteger(L, result);
    return 1;
}

static int l_system(lua_State* L)
{
    int result = bs_system(luaL_checkstring(L, 1));
    lua_pushinteger(L, result);
    return 1;
}

static const luaL_Reg bsexec[] = {
    {"mkdir", l_mkdir},
    {"listdir", l_listdir},
    {"exists", l_exists},
    {"spawn", l_spawn},
    {"copy", l_copy},
    {"copytree", l_copytree},
    {"system", l_system},
    {0, 0}
};

LUALIB_API int bs_open_bsexec(lua_State* L)
{
    luaL_register(L, "bsexec", bsexec);
    lua_pop(L, 1);
    return 0;
}
