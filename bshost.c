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

#include "bshost.h"
#include "bsunicode.h"
#include "bsdetect.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <libloaderapi.h>
#include <sys/stat.h>
#define getcwd _getcwd
#define stat _stat
#define PATH_MAX MAX_PATH

static int appPath(char* buf, int len)
{
    // TODO: ANSI to UTF-8
    return GetModuleFileNameA(NULL,buf,len);
}

// https://stackoverflow.com/questions/23427804/cant-find-mkdir-function-in-dirent-h-for-windows
int bs_mkdir(const char* normalizedPath)
{
    // TODO: is the Windows version expecting ANSI or UTF-8?
    return _mkdir(bs_denormalize_path(normalizedPath));
}

#else // Unix and the like
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

// https://stackoverflow.com/questions/933850/how-do-i-find-the-location-of-the-executable-in-c
static int appPath(char* buf, int len)
{
    ssize_t res = readlink("/proc/self/exe",buf,len-1);
    if( res < 0 )
    {
        res = readlink("/proc/curproc/file",buf,len-1);
        if( res < 0 )
        {
            res = readlink("/proc/self/path/a.out",buf,len-1);
            if( res < 0 )
                return -1;
        }
    }
    buf[res] = 0;
    return res;
}

// https://stackoverflow.com/questions/23427804/cant-find-mkdir-function-in-dirent-h-for-windows
int bs_mkdir(const char* normalizedPath)
{
    return mkdir(bs_denormalize_path(normalizedPath),0777); // returns 0 on success, -1 otherwise
}

#endif

static char s_buf[PATH_MAX];
// https://stackoverflow.com/questions/833291/is-there-an-equivalent-to-winapis-max-path-under-linux-unix

int bs_global_buffer_len() { return PATH_MAX; }
const char* bs_global_buffer() { return s_buf; }

BSPathStatus bs_normalize_path(const char* in, char* out, int outlen)
{
    assert( in && out && outlen > 0 );

    if( outlen < 10 )
        return BS_OutOfSpace;

    // skip whitespace
    while( *in != 0 && isspace(*in) )
        in++;
    if( *in == '~' )
        return BS_NotSupported;
    int inlen = strlen(in);
    if( inlen > 1 &&
            ( (in[0] == '/' && in[1] == '/') ||
              (in[0] == '\\' && in[1] == '\\') ))
        return BS_NotSupported;
    int i = 0; // points to next write position in 'out'
    int lastSlash = -1; // points to the last position of '/' in 'out'
    if( inlen > 1 && isalpha(in[0]) && in[1] == ':' )
    {
        // Path can start with a Windows root: e.g. "C:"
        out[i++] = '/';
        out[i++] = '/';
        out[i++] = in[0];
        out[i++] = in[1];
        in += 2;
        inlen -= 2;
        lastSlash = 1;
        if( inlen > 0 )
        {
            if(*in == '/' || *in == '\\')
            {
                lastSlash = i;
                out[i++] = '/';
                inlen--;
                in++;
            }else
            {
                uchar n = 0;
                const uint ch = unicode_decode_utf8((const uchar*) in, &n);
                if( n == 0 || bs_forbidden_fschar(ch) )
                    return BS_InvalidFormat;
                else
                    return BS_NotSupported;
            }
        }else
        {
            out[i] = 0;
            return BS_OK;
        }
    }else if( inlen > 0 && ( *in == '/' || *in == '\\' ) )
    {
        // Path can start with a Unix root '/'
        out[i++] = '/';
        out[i++] = '/';
        inlen--;
        in++;
        lastSlash = 1;
    }else if( inlen > 1 && in[0] == '.' && in[1] == '.' )
    {
        // Path can start with a sequence of '../../' or even be just '..'
        out[i++] = '.';
        out[i++] = '.';
        inlen -= 2;
        in += 2;
        while( inlen >= 3 && ( *in == '/' || *in == '\\' ) && in[1] == '.' && in[2] == '.' )
        {
            out[i++] = '/';
            out[i++] = '.';
            out[i++] = '.';
            if( i >= outlen )
                return BS_OutOfSpace;
            inlen -= 3;
            in += 3;
        }
        if( inlen == 0 )
        {
            out[i] = 0;
            return BS_OK;
        }else if( *in == '/' || *in == '\\' )
        {
            lastSlash = i;
            out[i++] = '/';
            inlen--;
            in++;
        }else
            return BS_InvalidFormat;
    }else if( inlen > 0 && in[0] == '.' )
    {
        // Path can start with './' or even be just '.'
        out[i++] = '.';
        inlen--;
        in++;
        if( inlen == 0 )
        {
            out[i] = 0;
            return BS_OK;
        }else if( *in == '/' || *in == '\\' )
        {
            lastSlash = i;
            out[i++] = '/';
            inlen--;
            in++;
        }else
            return BS_InvalidFormat;
    }else
    {
        // a relative path starting directly with an fsname; prefix out with './'
        uchar n = 0;
        const uint ch = unicode_decode_utf8((const uchar*) in, &n);
        if( n == 0 || bs_forbidden_fschar(ch) )
            return BS_InvalidFormat;
        out[i++] = '.';
        out[i++] = '/';
        lastSlash = 1;
    }
    assert( lastSlash >= 0 );
    int lastDot = 0;
    while( inlen )
    {
        uchar n = 0;
        uint ch = unicode_decode_utf8((const uchar*) in, &n);
        if( ch == '/' || ch == '\\' )
        {
            if( lastSlash )
            {
                const int diff = i - lastSlash;
                switch(diff)
                {
                case 1:
                    return BS_InvalidFormat;
                case 2:
                    if( strncmp(out+lastSlash,"/./",3) == 0 ||
                            strncmp(out+lastSlash,"\\.\\",3))
                        return BS_InvalidFormat;
                    break;
                case 3:
                    if( strncmp(out+lastSlash,"/../",4) == 0 ||
                            strncmp(out+lastSlash,"\\..\\",4) == 0 )
                        return BS_InvalidFormat;
                    break;
                }
            }
            lastSlash = i;
        }else if( ch == '.' )
        {
            if( lastDot && (i - lastDot) == 1 )
                return BS_InvalidFormat;
            lastDot = i;
        }else if( n == 0 || bs_forbidden_fschar(ch) )
            return BS_InvalidFormat;

        int j;
        for( j = 0; j < n; j++ )
        {
            if( *in == '\\' )
                out[i++] = '/';
            else
                out[i++] = in[j];
        }
        if( i >= outlen )
            return BS_OutOfSpace;
        inlen -= n;
        in += n;
    }
    out[i] = 0;
    if(i > 2 && ( out[i-1] == '/' || out[i-1] == '.') )
        return BS_InvalidFormat;
    return BS_OK;
}

const char* bs_denormalize_path(const char* path)
{
    assert(path != 0);
    if( *path == '/' )
    {
        assert(path[1] == '/');
        const int len = strlen(path);
        if( len >= 4 && path[3] == ':' )
            return path + 2;
        else
            return path + 1;
    }else
        return path;
}

BSPathStatus bs_normalize_path2(const char* in)
{
    return bs_normalize_path(in,s_buf,PATH_MAX-1);
}

// https://stackoverflow.com/questions/2868680/what-is-a-cross-platform-way-to-get-the-current-directory
BSPathStatus bs_cwd()
{
    char buffer[PATH_MAX];
    getcwd(buffer, sizeof(buffer));
    return bs_normalize_path2(buffer);
}

BSPathStatus bs_thisapp()
{
    char buffer[PATH_MAX];
    const int res = appPath(buffer, sizeof(buffer));
    if( res < 0 )
        return BS_NOP;
    else
        return bs_normalize_path2(buffer);
}

// https://stackoverflow.com/questions/7430248/creating-a-new-directory-in-c
// https://stackoverflow.com/questions/6094665/how-does-stat-under-windows-exactly-work
// https://stackoverflow.com/questions/230062/whats-the-best-way-to-check-if-a-file-exists-in-c
time_t bs_exists(const char* normalizedPath)
{
    struct stat st = {0};
    if( stat(bs_denormalize_path(normalizedPath), &st) == 0 )
        return st.st_mtime;
    else
        return 0;
}

int bs_forbidden_fschar(unsigned int ch)
{
    switch( ch )
    {
    case '\\':
    case '?':
    case '*':
    case '|':
    case '"':
    case '<':
    case '>':
    case ',':
    case ';':
    case '=':
    case '~':
        return 1;
    default:
        return 0;
    }
}

int bs_little_endian()
{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    return 1;
#else
    return 0;
#endif
}

const char* bs_host_os()
{
        #if defined(Q_OS_MACOS)
    return "macos";  // - macOS
        #elif defined(Q_OS_DARWIN)
    return "darwin";  // - Any Darwin system
        #elif defined(Q_OS_IOS)
    return "ios";  // - iOS
        #elif defined(Q_OS_MSDOS)
    return "msdos";  // - MS-DOS and Windows
        #elif defined(Q_OS_OS2)
    return "os2";  // - OS/2
        #elif defined(Q_OS_OS2EMX)
    return "os2emx";  // - XFree86 on OS/2 (not PM)
        #elif defined(Q_OS_WIN32)
    return "win32";  // - Win32 (Windows 2000/XP/Vista/7 and Windows Server 2003/2008)
        #elif defined(Q_OS_WINCE)
    return "wince";  // - WinCE (Windows CE 5.0)
        #elif defined(Q_OS_WINRT)
    return "winrt";  // - WinRT (Windows 8 Runtime)
        #elif defined(Q_OS_CYGWIN)
    return "cygwin";  // - Cygwin
        #elif defined(Q_OS_SOLARIS)
    return "solaris";  // - Sun Solaris
        #elif defined(Q_OS_HPUX)
    return "hpux";  // - HP-UX
        #elif defined(Q_OS_ULTRIX)
    return "ultrix";  // - DEC Ultrix
        #elif defined(Q_OS_LINUX)
    return "linux";  // - Linux [has variants]
        #elif defined(Q_OS_FREEBSD)
    return "freebsd";  // - FreeBSD [has variants]
        #elif defined(Q_OS_NETBSD)
    return "netbsd";  // - NetBSD
        #elif defined(Q_OS_OPENBSD)
    return "openbsd";  // - OpenBSD
        #elif defined(Q_OS_BSDI)
    return "bsdi";  // - BSD/OS
        #elif defined(Q_OS_INTERIX)
    return "interix";  // - Interix
        #elif defined(Q_OS_IRIX)
    return "irix";  // - SGI Irix
        #elif defined(Q_OS_OSF)
    return "osf";  // - HP Tru64 UNIX
        #elif defined(Q_OS_SCO)
    return "sco";  // - SCO OpenServer 5
        #elif defined(Q_OS_UNIXWARE)
    return "unixware";  // - UnixWare 7, Open UNIX 8
        #elif defined(Q_OS_AIX)
    return "aix";  // - AIX
        #elif defined(Q_OS_HURD)
    return "hurd";  // - GNU Hurd
        #elif defined(Q_OS_DGUX)
    return "dgux";  // - DG/UX
        #elif defined(Q_OS_RELIANT)
    return "reliant";  // - Reliant UNIX
        #elif defined(Q_OS_DYNIX)
    return "dynix";  // - DYNIX/ptx
        #elif defined(Q_OS_QNX)
    return "qnx";  // - QNX [has variants]
        #elif defined(Q_OS_QNX6)
    return "qnx6";  // - QNX RTP 6.1
        #elif defined(Q_OS_LYNX)
    return "lynx";  // - LynxOS
        #elif defined(Q_OS_BSD4)
    return "bsd4";  // - Any BSD 4.4 system
        #elif defined(Q_OS_UNIX)
    return "unix";  // - Any UNIX BSD/SYSV system
        #elif defined(Q_OS_ANDROID)
    return "android";  // - Android platform
        #elif defined(Q_OS_HAIKU)
    return "haiku";  // - Haiku
#else
#error "unknown operating system"
#endif
}

int bs_wordsize()
{
    return QT_POINTER_SIZE;
}

BSCpu bs_host_cpu()
{
    BSCpu res;
    res.name = "";
    res.ver = 0;
#if defined(Q_PROCESSOR_ARM)
    res.name = "arm";
    res.ver = Q_PROCESSOR_ARM;
#elif defined(Q_PROCESSOR_X86)
    res.name = "x86";
    res.ver = Q_PROCESSOR_X86;
#elif defined(Q_PROCESSOR_IA64)
    res.name = "ia64";
#elif defined(Q_PROCESSOR_MIPS)
    res.name = "mips";
#elif defined(Q_PROCESSOR_POWER)
    res.name = "ppc";
#elif defined(Q_PROCESSOR_S390)
    res.name = "s390";
#elif defined(Q_PROCESSOR_SPARC)
    res.name = "sparc";
#else
#error "unknown architecture"
#endif
    return res;
}

BSCompiler bs_host_compiler()
{
    BSCompiler res;
    res.name = "";
    res.ver = 0;
#if defined(Q_CC_SYM)
    res.name = "sym";   // - Digital Mars C/C++ (used to be Symantec C++)
#elif defined(Q_CC_MSVC)
    res.name = "msvc";   // - Microsoft Visual C/C++, Intel C++ for Windows
    res.ver = Q_CC_MSVC;
#elif defined(Q_CC_BOR)
    res.name = "bor";   // - Borland/Turbo C++
#elif defined(Q_CC_WAT)
    res.name = "wat";   // - Watcom C++
#elif defined(Q_CC_GNU)
    res.name = "gcc";   // - GNU C++
    res.ver = Q_CC_GNU;
#elif defined(Q_CC_COMEAU)
    res.name = "comeau";   // - Comeau C++
#elif defined(Q_CC_EDG)
    res.name = "edg";   // - Edison Design Group C++
#elif defined(Q_CC_OC)
    res.name = "oc";   // - CenterLine C++
#elif defined(Q_CC_SUN)
    res.name = "sun";   // - Forte Developer, or Sun Studio C++
#elif defined(Q_CC_MIPS)
    res.name = "mips";   // - MIPSpro C++
#elif defined(Q_CC_DEC)
    res.name = "dec";   // - DEC C++
#elif defined(Q_CC_HPACC)
    res.name = "hpacc";   // - HP aC++
#elif defined(Q_CC_USLC)
    res.name = "uslc";   // - SCO OUDK and UDK
#elif defined(Q_CC_CDS)
    res.name = "cds";   // - Reliant C++
#elif defined(Q_CC_KAI)
    res.name = "kay";   // - KAI C++
#elif defined(Q_CC_INTEL)
    res.name = "intel";   // - Intel C++ for Linux, Intel C++ for Windows
    res.ver = Q_CC_INTEL;
#elif defined(Q_CC_HIGHC)
    res.name = "highc";   // - MetaWare High C/C++
#elif defined(Q_CC_PGI)
    res.name = "pgi";   // - Portland Group C++
#elif defined(Q_CC_GHS)
    res.name = "ghs";   // - Green Hills Optimizing C++ Compilers
#elif defined(Q_CC_RVCT)
    res.name = "rvct";   // - ARM Realview Compiler Suite
#elif defined(Q_CC_CLANG)
    res.name = "clang";   // - C++ front-end for the LLVM compiler
    res.ver = Q_CC_CLANG;
#else
#error "unknown compiler"
#endif
    return res;
}

FILE* bs_fopen(const char* path, const char* modes)
{
    // TODO: Windows utf-8
    return fopen(path,modes);
}

int bs_exec(const char* cmd)
{
    // TODO convert from utf-8
    return system(cmd);
}

const char*bs_filename(const char* path)
{
    const int len = strlen(path);
    const char* p = path + len - 1;
    while( *p != '/' )
        p--;
    return p+1;
}

const char* bs_path_part(const char* path, BSPathPart what, int* len )
{
    // expecting a normalized path, i.e. starting with //, .. or .
    assert(len);

    const int plen = strlen(path);
    if( what == BS_all )
    {
        *len = plen;
        if( strncmp(path,"//",2) == 0 )
        {
            path = bs_denormalize_path(path);
            *len = strlen(path);
        }
        return path;
    }
    if( what == BS_fileName )
    {
        const char* q = path + plen - 1;
        const char* p = q;
        while( p > path && *p != '/' )
            p--;
        if( *p == '/' )
        {
            *len = q - p;
            return p + 1;
        }
    }
    if( what == BS_filePath )
    {
        const char* q = bs_path_part(path,BS_fileName, len);
        *len = q - path;
        if( q > path )
        {
            (*len)--; // trailing slash
            if( strncmp(path,"//",2) == 0 )
            {
                const char* tmp = bs_denormalize_path(path);
                *len -= tmp - path;
                path = tmp;
            }
            return path;
        }
    }
    if( what == BS_baseName )
    {
        const char* name = bs_path_part(path,BS_fileName, len);
        const char* q = name + *len - 1;
        const char* p = q;
        while( p > name && *p != '.' )
            p--;
        if( *p == '.' )
        {
            *len -= q - p + 1;
            return name;
        }else
            return name;
    }
    if( what == BS_extension )
    {
        const char* name = bs_path_part(path,BS_fileName, len);
        const char* q = name + *len - 1;
        const char* p = q;
        while( p > name && *p != '.' )
            p--;
        if( *p == '.' )
        {
            *len = q - p;
            return p + 1;
        }
    }

    *len = 0;
    return path;
}

static const char* path_part(const char* source, const char* what, int* len )
{
    assert(len);
    if( *len == 6 && strncmp(what,"source", 6) == 0 )
        return bs_path_part(source,BS_all,len);
    if( *len == 16 && strncmp(what,"source_file_part", 16) == 0 )
        return bs_path_part(source,BS_fileName,len);
    if( *len == 16 && strncmp(what,"source_name_part", 16) == 0 )
        return bs_path_part(source,BS_baseName,len);
    if( *len == 10 && strncmp(what,"source_dir", 10) == 0 )
        return bs_path_part(source,BS_filePath,len);
    if( *len == 10 && strncmp(what,"source_ext", 10) == 0 )
        return bs_path_part(source,BS_extension,len);
    *len = -1;
    return "";
}

BSPathStatus bs_apply_source_expansion(const char* source, const char* string)
{
    const char* s = string;
    char* p = s_buf;
    char* q = s_buf + PATH_MAX;
    while( *s && p < q )
    {
        if( *s == '{' && s[1] == '{' )
        {
            s += 2;
            if( *s == '}' )
                return BS_InvalidFormat;
            const char* start = s;
            while( *s != '}' && *s != 0 )
                s++;
            if( *s != '}' || s[1] != '}' )
                return BS_InvalidFormat;
            int len = s - start;
            const char* val = path_part(source,start, &len );
            if( len < 0 )
                return BS_NotSupported;
            if( p + len >= q )
                return BS_OutOfSpace;
            strncpy(p,val,len);
            s += 2;
            p += len;
        }else
        {
            *p = *s;
            p++;
            s++;
        }
    }
    *p = 0;
    return BS_OK;
}
