#ifndef BSLEX_H
#define BSLEX_H

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

#include "bslogger.h"

typedef struct BSLexer BSLexer;

typedef enum BSTokType {
    // generated with EbnfStudio from busy.ebnf
		Tok_Invalid = 0,

		TT_Literals,
		Tok_Bang,
		Tok_BangEq,
		Tok_Quote,
		Tok_Hash,
		Tok_2Hash,
		Tok_Dlr,
		Tok_Percent,
		Tok_Amp,
		Tok_2Amp,
		Tok_Lpar,
		Tok_Rpar,
		Tok_Star,
		Tok_Rcmt,
		Tok_StarEq,
		Tok_Plus,
		Tok_PlusEq,
		Tok_Comma,
		Tok_Minus,
		Tok_MinusEq,
		Tok_Dot,
		Tok_Slash,
		Tok_Lcmt,
		Tok_Colon,
		Tok_ColonEq,
		Tok_Semi,
		Tok_Lt,
		Tok_Leq,
		Tok_Eq,
		Tok_2Eq,
		Tok_Gt,
		Tok_Geq,
		Tok_Qmark,
		Tok_Lbrack,
		Tok_LbrackRbrack,
		Tok_Rbrack,
		Tok_Hat,
		Tok_60,
		Tok_Lbrace,
		Tok_2Bar,
		Tok_Rbrace,

		TT_Keywords,
		Tok_begin,
		Tok_class,
		Tok_define,
		Tok_else,
		Tok_elsif,
		Tok_end,
		Tok_false,
		Tok_if,
		Tok_import,
		Tok_in,
		Tok_include,
		Tok_let,
		Tok_param,
		Tok_subdir,
		Tok_submod,
		Tok_submodule,
		Tok_then,
		Tok_true,
		Tok_type,
		Tok_var,

		TT_Specials,
		Tok_ident,
		Tok_string,
		Tok_integer,
		Tok_real,
		Tok_path,
		Tok_symbol,
		Tok_Eof,

		TT_MaxToken,

		TT_Max
} BSTokType;

typedef struct BSToken {
    unsigned int tok: 8;
    unsigned int len: 24; // bytelen of val
    const char* val;
    BSRowCol loc;
    const char* source;
} BSToken;

typedef struct BSTokChain {
    BSToken tok;
    struct BSTokChain* next;
} BSTokChain;

extern BSLexer* bslex_open(const char* filepath, const char* sourceName); // filepath is utf-8 and denormalized
extern BSLexer* bslex_openFromString(const char* str, int len, const char* sourceName);
extern void bslex_free(BSLexer*);
extern void bslex_mute(BSLexer*);
extern void bslex_alltokens(BSLexer*);
extern BSToken bslex_next(BSLexer*);
extern BSToken bslex_peek(BSLexer*, int off); // off = 1..
extern const char* bslex_filepath(BSLexer*);
extern void bslex_dump(const BSToken*);
extern const char* bslex_tostring(int tok);
extern int bslex_numOfUnichar(const char*, int len);
extern void bslex_setlogger(BSLexer*,BSLogger,void*);
extern void bs_defaultLogger(BSLogLevel l, void* data,const char* file, BSRowCol loc, const char* format, va_list args);

typedef struct BSHiLex BSHiLex; // hierarchic lexer

extern BSHiLex* bslex_createhilex(const char* filepath, const char* sourceName);
extern void bslex_freehilex(BSHiLex*);
extern BSToken bslex_hnext(BSHiLex*);
extern void bslex_cursetref(BSHiLex*);
extern BSToken bslex_hpeek(BSHiLex*, int off); // off = 1..
extern int bslex_hopen(BSHiLex*, const char* str, int len, const char* sourceName, BSRowCol orig);
extern void bslex_addarg(BSHiLex*, const char* name, BSTokChain* what); // ownership of what is transferred
extern const char* bslex_hfilepath(BSHiLex*);
extern int bslex_hlevelcount(BSHiLex*);
extern BSToken bslex_hlevel(BSHiLex*, unsigned int level);
extern char* bslex_allocstr(BSHiLex*,int len);
extern void bslex_hsetlogger(BSHiLex*,BSLogger,void*);


#endif // BSLEX_H
