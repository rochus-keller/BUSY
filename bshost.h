#ifndef BSHOST_H
#define BSHOST_H

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

#include <stdio.h>
#include <time.h>

extern FILE* bs_fopen(const char* path, const char* modes);
extern const char* bs_global_buffer();
extern int bs_global_buffer_len();
extern time_t bs_exists(const char* normalizedPath); // changed time if exists, 0 if not
extern int bs_mkdir(const char* normalizedPath);
extern int bs_exec(const char* cmd);

typedef enum BSPathStatus { BS_OK, BS_NotSupported, BS_InvalidFormat, BS_OutOfSpace, BS_NOP } BSPathStatus;
extern BSPathStatus bs_normalize_path(const char* in, char* out, int outlen); // OS path to normalized path
extern BSPathStatus bs_normalize_path2(const char* in);
extern BSPathStatus bs_cwd();
extern BSPathStatus bs_thisapp();
extern BSPathStatus bs_apply_source_expansion(const char* source, const char* string);
extern const char* bs_denormalize_path(const char* path);
typedef enum BSPathPart { BS_all, BS_fileName, BS_filePath, BS_baseName, BS_extension } BSPathPart;
extern const char* bs_path_part(const char* path, BSPathPart what, int* len );
extern const char* bs_filename(const char* path);
extern int bs_forbidden_fschar(unsigned int ch);
extern int bs_little_endian(); // 1..little, 0..big
extern int bs_wordsize(); // number of bytes
extern const char* bs_host_os();

typedef struct BSCpu {
    const char* name;
    int ver;
} BSCpu;
extern BSCpu bs_host_cpu();

typedef struct BSCompiler {
    const char* name;
    int ver;
}BSCompiler;
extern BSCompiler bs_host_compiler();
// NOTE that the idea is to bootstrap this build system before comiling the products with the same toolchain
// so host os, cpu and compiler are known when the build is run. Cross compilation is not yet supported.

#endif // BSHOST_H
