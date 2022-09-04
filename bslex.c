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

#include "bslex.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include "bsunicode.h"
#include "bshost.h"

typedef struct BSTokenQueue {
    BSToken tok;
    struct BSTokenQueue* next;
} BSTokenQueue;

typedef struct BSLexer
{
    const char* source;
    const uchar* str;
    const uchar* pos;
    const uchar* end;
    uint ch;
    BSRowCol loc;
    BSTokenQueue* first;
    BSTokenQueue* last;
    uchar n; // number of bytes read for cur ch
    uchar ownsStr;
} BSLexer;

static void readchar(BSLexer* l)
{
    if( l->n )
    {
        l->pos += l->n;
        l->loc.col++;
    }

    if( l->pos >= l->end )
    {
        l->n = 0;
        l->ch = 0;
    }else
    {
        l->ch = unicode_decode_utf8(l->pos, &l->n);
        if( l->n == 0 || l->n > (l->end - l->pos) )
        {
            fprintf(stderr, "file has invalid utf-8 format: %s\n", l->source);
            exit(1);
        }
    }
}

BSLexer* bslex_open(const char* filepath, const char* sourceName)
{
    FILE* f = bs_fopen(filepath,"r");
    if( f == NULL )
    {
        fprintf(stderr, "cannot open file for reading %s\n", filepath);
        return 0;
    }
    fseek(f, 0L, SEEK_END);
    const int sz = ftell(f);
    if( sz < 0 )
    {
        fprintf(stderr, "cannot determine file size %s\n", filepath);
        return 0;
    }
    rewind(f);

    uchar* str = (uchar*) malloc(sz+1);
    if( str == NULL )
    {
        fprintf(stderr, "not enough memory to read file %s\n", filepath);
        return 0;
    }

    if( fread( str, 1, sz, f ) != (size_t)sz )
    {
        free(str);
        fprintf(stderr, "cannot read file %s\n", filepath);
        return 0;
    }
    str[sz] = 0;

    BSLexer* l = (BSLexer*) calloc(1,sizeof(BSLexer));
    if( l == NULL )
    {
        free(str);
        fprintf(stderr, "not enough memory to process file %s\n", filepath);
        return 0;
    }

    if( sourceName == 0 )
        l->source = filepath;
    else
        l->source = sourceName;
    l->str = str;
    l->pos = str;
    l->end = str + sz;
    l->loc.row = 1;
    l->loc.col = 0;
    l->ownsStr = 1;

    if( sz >= 3 && str[0] == 0xEF && str[1] == 0xBB && str[2] == 0xBF)
        l->pos += 3; // remove BOM

    readchar(l);

    return l;
}

BSLexer* bslex_openFromString(const char* str, int len, const char* sourceName)
{
    BSLexer* l = (BSLexer*) calloc(1,sizeof(BSLexer));
    if( l == NULL )
    {
        fprintf(stderr, "not enough memory create BSLexer\n");
        return 0;
    }
    const int sz = len;
    l->source = sourceName;
    l->str = (const uchar*)str;
    l->pos = l->str;
    l->end = l->str + sz;
    l->loc.row = 1;
    l->loc.col = 0;
    l->ownsStr = 0;

    if( sz >= 3 && l->str[0] == 0xEF && l->str[1] == 0xBB && l->str[2] == 0xBF)
        l->pos += 3; // remove BOM

    readchar(l);

    return l;
}

void bslex_free(BSLexer* l)
{
    if( l == NULL )
        return;
    assert(l);
    assert(l->str);
    while( l->first )
    {
        BSTokenQueue* tmp = l->first;
        l->first = l->first->next;
        free(tmp);
    }
    if( l->ownsStr )
        free((uchar*)l->str);
    free(l);
}

static char at( const char* str, int i ){
    return ( i >= 0 ? str[i] : 0 );
}

static BSTokType tokenTypeFromString( const char* str, int* pos );

static void skipWhiteSpace(BSLexer* l)
{
    while( l->pos < l->end && ( isspace(l->ch) || unicode_isspace(l->ch) ) )
    {
        if( l->ch == '\n' )
        {
            l->loc.row++;
            l->loc.col = 0;
        }
        readchar(l);
    }
}

static void error(BSLexer* l, const char* format, ...)
{
    fprintf(stderr,"%s:%d:%d: ", l->source, l->loc.row, l->loc.col );
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

static BSToken path(BSLexer* l, BSToken t, int quoted )
{
    // path ::= [ '../' { '../' } | './' | '//' ] fsname { '/' fsname } | '..' | '.'
    // fsname := fschar { fschar }
    // fschar ::= any unicode char besides '/', '\', '?', '*', '|', '"', '<', '>', ',', ';', '='
    //   fschar can include ':' directly after '//' and a single letter, like "//c:/"
    //   fschar can include ' ' if path is surrounded by '\''
    // path may not include '/../' or '/./' nor any pair of '..'

    // when we arrive here we're at the start or somewhere within the first fsname
    t.tok = Tok_path;

    if( quoted )
    {
        assert( t.val == (const char*)l->pos && *t.val == '\'' );
        readchar(l);
        if( l->ch == '.' )
        {
            readchar(l);
            if( l->ch == '.' )
            {
                readchar(l);
                if( l->ch != '/' && l->ch != '\'' )
                {
                    error(l, "expecting '..' or '../path'\n" );
                    t.tok = Tok_Invalid;
                    return t;
                }
            }else if( l->ch != '/' && l->ch != '\'' )
            {
                error(l, "expecting '.' or './path'\n" );
                t.tok = Tok_Invalid;
                return t;
            }
        }else if( l->ch == '/' )
        {
            readchar(l);
            if( l->ch != '/' )
            {
                error(l, "expecting '//' in the root of an absolute path\n" );
                t.tok = Tok_Invalid;
                return t;
            }
        }else if( l->ch == '\'' )
        {
            error(l, "empty paths not allowed\n" );
            t.tok = Tok_Invalid;
            return t;
        }
    }

    int dotdotStart = strncmp( quoted ? t.val+1: t.val,"../",3) == 0;

    const char* lastSlash = 0;
    const char* i;
    for( i = (const char*)l->pos; i >= t.val; i-- )
    {
        if( *i == '/' )
        {
            lastSlash = i;
            break;
        }
    }

    const char* lastDot = 0;
    while( l->pos < l->end )
    {
        const char ch = l->ch;
        if( quoted && l->ch == '\\' && l->pos[1] == '\'' ) // only one escape is supported: \'
            readchar(l);
        else if( bs_forbidden_fschar(l->ch) )
        {
            error( l, "cannot use '%c' in a path\n", l->ch );
            t.tok = Tok_Invalid;
            return t;
        }else if( l->ch == ':' )
        {
            const char* start = quoted ? t.val + 1 : t.val;
            const int diff = (const char*)l->pos - start;
            if( !(  diff == 3
                    && start[0] == '/'
                    && start[1] == '/'
                    && isalpha(start[2])
                    && ( l->pos[1] == '/' || isspace(l->pos[1]) || ( quoted && l->pos[1] == '\'' ) ) ) )
            {
                error( l, "':' can only be used in the root of an absolute path like //c:\n" );
                t.tok = Tok_Invalid;
                return t;
            }
        }else if( l->ch == '/' )
        {
            if( lastSlash )
            {
                const int diff = (const char*)l->pos - lastSlash;
                switch(diff)
                {
                case 1:
                    error(l, "'//' only allowed at the beginning of an absolute path\n");
                    t.tok = Tok_Invalid;
                    return t;
                case 2:
                    if( strncmp(lastSlash,"/./",3) == 0 )
                    {
                        error(l, "'/./' not allowed in a path\n" );
                        t.tok = Tok_Invalid;
                        return t;
                    }
                    break;
                case 3:
                    if( strncmp(lastSlash,"/../",4) == 0 )
                    {
                        if( !dotdotStart )
                        {
                            error(l, "'/../' not allowed in a path\n" );
                            t.tok = Tok_Invalid;
                            return t;
                        }
                    }
                    break;
                }
            }
            lastSlash = (const char*)l->pos;
        }else if( l->ch == '.' )
        {
            if( !dotdotStart && lastDot && ((const char*)l->pos - lastDot) == 1 )
            {
                error(l, "pairs of '..' not allowed in a path\n" );
                t.tok = Tok_Invalid;
                return t;
            }
            lastDot = (const char*)l->pos;
        }else if( !quoted && ( isspace(l->ch) || unicode_isspace(l->ch) ) )
            break;
        else if( quoted && l->ch == '\'' )
        {
            readchar(l);
            break;
        }else
            dotdotStart = 0;
        readchar(l);
    }

    if( lastSlash && (const char*)l->pos - lastSlash == ( quoted ? 2 : 1 ) &&
           (const char*)l->pos - t.val > ( quoted ? 3 : 2 ) )
    {
        error(l, "trailing '/' not allowed\n" );
        t.tok = Tok_Invalid;
        return t;
    }
    t.len = (const char*)l->pos - t.val;
    return t;
}

static BSToken ident(BSLexer* l, BSToken t)
{
    t.val = (const char*)l->pos;
    t.loc = l->loc;
    do
    {
        readchar(l);
    }while( unicode_isletter(l->ch) || l->ch == '_' || unicode_isdigit(l->ch) );

    if( 0 ) // no, since ident.ident, ident-ident or ident/ident has a meaning: l->ch == '.' || l->ch == '/' || l->ch == '-' )
    {
        // it's a relative path not starting with "./", i.e. looks like an ident until here
        return path(l,t, 0);
    }

    t.tok = Tok_ident;
    t.len = (const char*)l->pos - t.val;

    int pos = 0;
    const BSTokType kw = tokenTypeFromString(t.val,&pos);
    if( kw != Tok_Invalid && pos == t.len )
        t.tok = kw;
    return t;
}

static BSToken symbol(BSLexer* l, BSToken t)
{
    t.val = (const char*)l->pos;
    t.loc = l->loc;
    do
    {
        readchar(l);
    }while( unicode_isletter(l->ch) || l->ch == '_' || unicode_isdigit(l->ch) );

    t.tok = Tok_symbol;
    t.len = (const char*)l->pos - t.val;

    return t;
}

static int ishexdigit(int ch)
{
    return isdigit(ch) || ( ch >= 'A' && ch <= 'F' ) || ( ch >= 'a' && ch <= 'f' );
}

static BSToken number(BSLexer* l,BSToken t)
{
    t.val = (const char*)l->pos;
    t.loc = l->loc;

    // integer ::= // digit {digit}
    // hex ::= '0x' hexDigit {hexDigit}
    // real ::= // digit {digit} '.' {digit} [ScaleFactor]
    // ScaleFactor- ::= // 'e' ['+' | '-'] digit {digit}

    readchar(l);

    if( *t.val == '0' && ( l->ch == 'x' || l->ch == 'X' ) )
    {
        // read a hex integer
        readchar(l);
        while( ishexdigit(l->ch) )
            readchar(l);
        t.tok = Tok_integer;
        t.len = (const char*)l->pos - t.val;
        return t;
    }

    while( unicode_isdigit(l->ch) )
        readchar(l);

    if( l->ch == '.' || l->ch == 'e' || l->ch == 'E' )
    {
        if( l->ch == '.' )
        {
            readchar(l);
            if( !unicode_isdigit(l->ch) )
                error(l, "expecting a digit after '.'\n" );
            while( unicode_isdigit(l->ch) )
                readchar(l);
        }
        if( l->ch == 'e' || l->ch == 'E' )
        {
            readchar(l);
            if( l->ch == '+' || l->ch == '-' )
                readchar(l);
            if( !unicode_isdigit(l->ch) )
                error(l, "expecting a digit after exponent\n" );
            while( unicode_isdigit(l->ch) )
                readchar(l);

        }
        t.tok = Tok_real;
        t.len = (const char*)l->pos - t.val;
        return t;
    }else
    {
        t.tok = Tok_integer;
        t.len = (const char*)l->pos - t.val;
        return t;
    }
}

static BSToken string(BSLexer* l,BSToken t)
{
    t.val = (const char*)l->pos;
    t.loc = l->loc;

    while( l->pos < l->end )
    {
        readchar(l);
        if( l->ch == '\n' )
        {
            l->loc.row++;
            l->loc.col = 0;
        }else if( l->ch == '\\' )
        {
            if( l->pos[1] == '"' || l->pos[1] == '\\' ) // valid escapes are \\ and \"
                readchar(l);
        }else if( l->ch == '"' )
            break;
    }
    if( l->ch != '"' )
    {
        error(l,"non-terminated string\n" );
        t.tok = Tok_Invalid;
        return t;
    }else
        readchar(l);
    t.tok = Tok_string;
    t.len = (const char*)l->pos - t.val;
    return t;
}

static void comment(BSLexer* l)
{
    // '/*' already read

    int level = 1;
    while( l->pos < l->end )
    {
        readchar(l);
        if( l->ch == '*' )
        {
            readchar(l);
            if( l->ch == '/' )
                level--;
        }else if( l->ch == '/' )
        {
            readchar(l);
            if( l->ch == '*' )
                level++;
        }
        if( l->ch == '\n' )
        {
            l->loc.row++;
            l->loc.col = 0;
        }
        if( level == 0 )
            break;
    }
    readchar(l);
    if( level != 0 )
        error(l,"non-terminated comment\n" );
}

static BSToken next(BSLexer* l)
{
    assert(l != NULL);

    skipWhiteSpace(l);

    BSToken t;
    memset(&t,0,sizeof(BSToken));
    t.tok = Tok_Eof;
    t.val = (const char*)l->pos;
    t.loc = l->loc;
    t.source = l->source;

    if( l->pos >= l->end ) // reached eof
        return t;

    if( unicode_isletter(l->ch) || l->ch == '_' )
        return ident(l,t);
    if( unicode_isdigit(l->ch) )
        return number(l,t);
    switch( l->ch )
    {
    case '\'':
        t = path(l, t, 1 );
        break;
    case '.':
        readchar(l);
        switch(l->ch)
        {
        case '.':
            readchar(l);
            if( l->ch == '/' )
            {
                readchar(l);
                t = path(l, t, 0 );
            }else if( isspace(l->ch) )
            {
                t.tok = Tok_path;
                t.len = 2;
            }else
            {
                error(l, "expecting '/' after '..'\n" );
                t.tok = Tok_Invalid;
            }
            break;
        case '/':
            readchar(l);
            t = path(l, t, 0 );
            break;
        default:
            t.tok = isspace(l->ch) ? Tok_path : Tok_Dot;
            t.len = 1;
            break;
        }
        break;
    case '/':
        readchar(l);
        if( l->ch == '/' )
        {
            readchar(l);
            t = path(l, t, 0 );
        }else if( l->ch == '*')
        {
            readchar(l);
            comment(l);
            t = next(l);
       }else
        {
            t.tok = Tok_Slash;
            t.len = 1;
        }
        break;
    case '#': // line comment, read until end of line
        while(l->pos < l->end && l->ch != '\n')
            readchar(l);
        t = next(l);
        break;
    case '"':
        return string(l,t);
    case '`':
    case '$': // TODO: we currently accept both
        return symbol(l, t);
    default:
        {
            int pos = 0;
            t.tok = tokenTypeFromString(t.val,&pos);
            if( t.tok == Tok_Invalid )
            {
                assert( pos == 0 );
                pos++;
                if( t.val )
                    error(l, "unexpected symbol: %c\n", *t.val );
                else
                    error(l, "unexpected symbol\n" );
            }
            t.len = pos;
            while( pos-- > 0 )
                readchar(l);
        }
        break;
    }

    return t;
}

BSToken bslex_next(BSLexer* l)
{
    if( l->first && l->first->tok.tok != Tok_Invalid )
    {
        BSToken res = l->first->tok;
        l->first->tok.tok = Tok_Invalid;
        if( l->first != l->last )
        {
            BSTokenQueue* tmp = l->first;
            l->first = tmp->next;
            l->last->next = tmp;
            l->last = tmp;
            tmp->next = 0; // necessary because of deleteQueue
        }
        return res;
    }else
        return next(l);
}

BSToken bslex_peek(BSLexer* l, int off )
{
    assert( off > 0 );
    off--;
    BSTokenQueue* tmp = l->first;
    int i = 0;
    while( tmp && tmp->tok.tok != Tok_Invalid && i != off )
    {
        tmp = tmp->next;
        i++;
    }
    while( i != off || tmp == 0 || tmp->tok.tok == Tok_Invalid )
    {
        if( tmp == 0 )
        {
            tmp = (BSTokenQueue*)malloc(sizeof(BSTokenQueue));
            if( tmp == 0 )
            {
                fprintf(stderr, "not enough memory to read file %s\n", l->source);
                exit(1);
            }
            tmp->tok = next(l);
            tmp->next = 0;
            if( l->first == 0 )
            {
                l->first = l->last = tmp;
            }else
            {
                l->last->next = tmp;
                l->last = tmp;
            }
        }else if( tmp->tok.tok == Tok_Invalid )
        {
            tmp->tok = next(l);
            if( tmp->tok.tok == Tok_Invalid )
                return tmp->tok;
        }
        if( i != off )
        {
            tmp = tmp->next;
            i++;
        }
    }
    assert( i == off && tmp != 0 );
    return tmp->tok;
}

void bslex_dump(const BSToken* t)
{
    char buf[16];
    int i;
    for(i = 0; i < (t->len < 15 ? t->len : 15); i++ )
    {
        if( isspace(t->val[i]) )
            buf[i] = ' ';
        else
            buf[i] = t->val[i];
    }
    buf[i] = 0;
    printf("Tok %d:%d:%d %s\n", t->tok, t->loc.row, t->loc.col, buf );
    fflush(stdout);
}

const char*bslex_filepath(BSLexer* lex)
{
    return lex->source;
}

typedef struct BSTemplArg
{
    BSTokChain* what;
    const char* name;
    struct BSTemplArg* next;
} BSTemplArg;

typedef struct BSHiLexLevel
{
    BSLexer* lex;
    BSRowCol orig; // the place where the string in lex is located in the file
    BSToken ref;
    BSTemplArg* args;
}BSHiLexLevel;

typedef struct BSHiLexMemSlot
{
    struct BSHiLexMemSlot* next;
    char str[sizeof(void*)];
} BSHiLexMemSlot;

typedef struct BSHiLex
{
#define BS_MAX_LEVEL  20
    BSHiLexLevel lex[BS_MAX_LEVEL];
    unsigned level : 5;
    BSTokenQueue* first;
    BSTokenQueue* last;
    BSToken cur;
    BSHiLexMemSlot* mem;
} BSHiLex;

BSHiLex*bslex_createhilex(const char* filepath, const char* sourceName)
{
    BSLexer* l = bslex_open(filepath,sourceName);
    if( l == 0 )
        return 0;
    BSHiLex* hl = (BSHiLex*)calloc(1,sizeof(BSHiLex));
    if( hl == 0 )
    {
        bslex_free(l);
        fprintf(stderr, "not enough memory to create lexer for %s\n", filepath);
        return 0;
    }
    hl->lex[0].lex = l;
    return hl;
}

static void freeLevel(BSHiLexLevel* l)
{
    while( l->args )
    {
        BSTemplArg* tmp = l->args;
        while(tmp->what)
        {
            BSTokChain* tmp2 = tmp->what;
            tmp->what = tmp->what->next;
            free(tmp2);
        }
        l->args = l->args->next;
        free(tmp);
    }
    bslex_free(l->lex);
}

void bslex_freehilex(BSHiLex* l)
{
    if( l == NULL )
        return;
    int i;
    for( i = 0; i <= l->level; i++ )
    {
        freeLevel(&l->lex[i]);
    }
    while( l->mem )
    {
        BSHiLexMemSlot* tmp = l->mem;
        l->mem = l->mem->next;
        free(tmp);
    }
    free(l);
}

static int isequal( const char* lhs, const char* rhs, int len)
{
    int i = 0;
    while( *lhs && i < len )
    {
        if( *lhs != rhs[i] )
            return 0;
        i++;
        lhs++;
    }
    return i == len;
}

static char* chainToStr(BSHiLex* l, BSTokChain* ts)
{
    assert(ts);
    BSTokChain* p = ts;
    int len = 0;
    while(p)
    {
        len += p->tok.len + 1; // +1 for space
        p = p->next;
    }
    char* res = bslex_allocstr(l,len+1);
    if( res == 0 )
    {
        fprintf(stderr,"%s:%d:%d:ERR: not enough memory to make string of token chain\n",
                ts->tok.source, ts->tok.loc.row, ts->tok.loc.col );
        exit(1);
    }
    p = ts;
    char* s = res;
    while(p)
    {
        strncpy(s,p->tok.val,p->tok.len);
        s += p->tok.len;
        *s = ' ';
        s++;
        p = p->next;
    }
    *s = 0;
    return res;
}

static BSToken hnext(BSHiLex* l)
{
    BSHiLexLevel* lev = &l->lex[l->level];
    BSToken t = bslex_next(lev->lex);

    if( t.tok == Tok_Eof )
    {
        if( l->level > 0 )
        {
            freeLevel(lev);
            l->level--;
            lev = &l->lex[l->level];
            t = bslex_next(lev->lex);
        }
    }

    if( t.tok == Tok_ident && lev->args )
    {
        // if ident check for arg stream
        BSTemplArg* a = lev->args;
        while(a)
        {
            // TODO: should that be more efficient?
            if( isequal(a->name, t.val, t.len) )
            {
                // the first is just to transport loc and source if tok == 0
                char* str = chainToStr(l,a->what->tok.tok == 0 ? a->what->next : a->what);
                // TODO: support directly lexing token streams instead of strings
                bslex_hopen(l,str, strlen(str), a->what->tok.source, a->what->tok.loc);
                lev = &l->lex[l->level];
                t = bslex_next(lev->lex);
                break;
            }
            a = a->next;
        }
    }

    if( lev->orig.col )
        t.loc.col += t.loc.row==1 ? lev->orig.col:0;
    if( lev->orig.row )
        t.loc.row += lev->orig.row - 1;

    return t;
}

BSToken bslex_hnext(BSHiLex* l)
{
    BSToken t = bslex_hpeek(l,1); // no direct calls to hnext so that
                      // bslex_hpeek can take care of the ident concat operator.
                      // no additional cost since BSTokenQueue is reused
    if( t.tok == Tok_Invalid )
        return t;
    assert(l->first && l->first->tok.tok != Tok_Invalid);

    l->cur = l->first->tok;
    l->first->tok.tok = Tok_Invalid;
    if( l->first != l->last )
    {
        BSTokenQueue* tmp = l->first;
        l->first = tmp->next;
        l->last->next = tmp;
        l->last = tmp;
        tmp->next = 0; // necessary because of deleteQueue
    }

    return l->cur;
}

static BSTokenQueue* reserveSlots(BSHiLex* l, BSTokenQueue* firstInvalid, int n )
{
    BSTokenQueue* tmp = firstInvalid;
    assert( tmp == 0 || tmp->tok.tok == Tok_Invalid );

    int i = 0;
    while( i <= n )
    {
        if( tmp == 0 )
        {
            tmp = (BSTokenQueue*)calloc(1,sizeof(BSTokenQueue));
            if( tmp == 0 )
            {
                fprintf(stderr, "not enough memory to allocate token queue for %s\n", l->lex[0].lex->source);
                exit(1);
            }
            if( firstInvalid == 0 )
                firstInvalid = tmp;
            if( l->first == 0 )
            {
                l->first = l->last = tmp;
            }else
            {
                l->last->next = tmp;
                l->last = tmp;
            }
        }
        tmp = tmp->next;
        i++;
    }
    return firstInvalid;
}

BSToken bslex_hpeek(BSHiLex* l, int off)
{
    assert( off > 0 );
    off--;
    BSTokenQueue* tmp = l->first;
    int i = 0;
    while( tmp && tmp->tok.tok != Tok_Invalid && i != off )
    {
        tmp = tmp->next;
        i++;
    }
    if( i == off && tmp != 0 && tmp->tok.tok != Tok_Invalid )
        return tmp->tok;

    if( i < off || tmp == 0 )
        tmp = reserveSlots(l, tmp, off - i + 2); // + 2 because of ident concat check

    assert( i <= off && tmp != 0 && tmp->tok.tok == Tok_Invalid );
    // peek has to fetch tokens
    BSTokenQueue* res = tmp;
    while( i < off || ( tmp && tmp->tok.tok == Tok_Invalid ) )
    {
        assert( tmp && tmp->tok.tok == Tok_Invalid );
        tmp->tok = hnext(l);
        res = tmp;
        if( tmp->tok.tok == Tok_Invalid )
            return tmp->tok; // premature end because of error
        if( tmp->tok.tok == Tok_ident )
        {
            BSToken a = hnext(l);
            if( a.tok == Tok_Amp )
            {
                BSToken b = hnext(l);
                if( b.tok == Tok_ident )
                {
                    // TODO: this simply makes a new copy even if same ident
                    char* str = bslex_allocstr(l,tmp->tok.len + b.len);
                    if( str == 0 )
                    {
                        fprintf(stderr,"%s:%d:%d:ERR: not enough memory to copy identifier\n",
                                a.source, a.loc.row, a.loc.col );
                        exit(1);
                    }
                    strncpy(str,tmp->tok.val,tmp->tok.len);
                    strncpy(str+tmp->tok.len,b.val,b.len);
                    tmp->tok.val = str;
                    tmp->tok.len += b.len;
                }else
                {
                    fprintf(stderr, "%s:%d:%d:ERR: operator '&' requires an identifier on left and right side\n",
                            a.source,a.loc.row, a.loc.col);
                    res->tok.tok = Tok_Invalid;
                    return res->tok;
                }
            }else
            {
                tmp = tmp->next;
                assert( tmp != 0 );
                tmp->tok = a;
                if( i < off )
                {
                    res = tmp;
                    i++;
                }
            }
        }
        if( i < off )
        {
            tmp = tmp->next;
            i++;
        }
    }
    assert( i >= off && res != 0 );
    return res->tok;
}

int bslex_hopen(BSHiLex* l, const char* str, int len, const char* sourceName, BSRowCol orig)
{
    if( l->level+1 >= BS_MAX_LEVEL )
    {
        fprintf(stderr, "%s:%d:%d:ERR lexer stack: maximum levels reached (%d levels)\n", sourceName,
                orig.row, orig.col, BS_MAX_LEVEL);
        exit(1);
        return 0;
    }
    if( l->lex[l->level].ref.tok == Tok_Invalid )
        l->lex[l->level].ref = l->cur;
    l->level++;
    l->lex[l->level].lex = bslex_openFromString(str, len, sourceName);
    l->lex[l->level].orig = orig;
    return 1;
}

const char*bslex_hfilepath(BSHiLex* l)
{
    return bslex_filepath(l->lex[l->level].lex);
}

int bslex_hlevelcount(BSHiLex* l)
{
    return l->level + 1;
}

BSToken bslex_hlevel(BSHiLex* l, unsigned int level)
{
    return l->lex[level].ref;
}

void bslex_cursetref(BSHiLex* l)
{
    l->lex[l->level].ref = l->cur;
}

void bslex_addarg(BSHiLex* l, const char* name, BSTokChain* what)
{
    BSTemplArg* arg = (BSTemplArg*)malloc(sizeof(BSTemplArg));
    arg->next = l->lex[l->level].args;
    arg->name = name;
    arg->what = what;
    l->lex[l->level].args = arg;
}

const char*bslex_tostring(int tok)
{
    const int r = tok;
    // generated with EbnfStudio from busy.ebnf
    switch(r) {
    case Tok_Invalid: return "<invalid>";
    case Tok_Bang: return "!";
    case Tok_BangEq: return "!=";
    case Tok_Hash: return "#";
    case Tok_2Hash: return "##";
    case Tok_Dlr: return "$";
    case Tok_Percent: return "%";
    case Tok_Amp: return "&";
    case Tok_2Amp: return "&&";
    case Tok_Lpar: return "(";
    case Tok_Rpar: return ")";
    case Tok_Star: return "*";
    case Tok_Rcmt: return "*/";
    case Tok_StarEq: return "*=";
    case Tok_Plus: return "+";
    case Tok_PlusEq: return "+=";
    case Tok_Comma: return ",";
    case Tok_Minus: return "-";
    case Tok_MinusEq: return "-=";
    case Tok_Dot: return ".";
    case Tok_Slash: return "/";
    case Tok_Lcmt: return "/*";
    case Tok_Colon: return ":";
    case Tok_ColonEq: return ":=";
    case Tok_Semi: return ";";
    case Tok_Lt: return "<";
    case Tok_Leq: return "<=";
    case Tok_Eq: return "=";
    case Tok_2Eq: return "==";
    case Tok_Gt: return ">";
    case Tok_Geq: return ">=";
    case Tok_Qmark: return "?";
    case Tok_Lbrack: return "[";
    case Tok_LbrackRbrack: return "[]";
    case Tok_Rbrack: return "]";
    case Tok_Hat: return "^";
    case Tok_60: return "`";
    case Tok_Lbrace: return "{";
    case Tok_2Bar: return "||";
    case Tok_Rbrace: return "}";
    case Tok_begin: return "begin";
    case Tok_class: return "class";
    case Tok_define: return "define";
    case Tok_else: return "else";
    case Tok_elsif: return "elsif";
    case Tok_end: return "end";
    case Tok_false: return "false";
    case Tok_if: return "if";
    case Tok_import: return "import";
    case Tok_in: return "in";
    case Tok_include: return "include";
    case Tok_let: return "let";
    case Tok_param: return "param";
    case Tok_subdir: return "subdir";
    case Tok_then: return "then";
    case Tok_true: return "true";
    case Tok_type: return "type";
    case Tok_var: return "var";
    case Tok_ident: return "ident";
    case Tok_string: return "string";
    case Tok_integer: return "integer";
    case Tok_real: return "real";
    case Tok_path: return "path";
    case Tok_symbol: return "symbol";
    case Tok_Eof: return "<eof>";
    default: return "";
    }
}

static BSTokType tokenTypeFromString( const char* str, int* pos )
{
    // generated with EbnfStudio from busy.ebnf
    int i = ( pos != 0 ? *pos: 0 );
    BSTokType res = Tok_Invalid;
    switch( at(str,i) ){
    case '!':
        if( at(str,i+1) == '=' ){
            res = Tok_BangEq; i += 2;
        } else {
            res = Tok_Bang; i += 1;
        }
        break;
    case '#':
        if( at(str,i+1) == '#' ){
            res = Tok_2Hash; i += 2;
        } else {
            res = Tok_Hash; i += 1;
        }
        break;
    case '$':
        res = Tok_Dlr; i += 1;
        break;
    case '%':
        res = Tok_Percent; i += 1;
        break;
    case '&':
        if( at(str,i+1) == '&' ){
            res = Tok_2Amp; i += 2;
        } else {
            res = Tok_Amp; i += 1;
        }
        break;
    case '(':
        res = Tok_Lpar; i += 1;
        break;
    case ')':
        res = Tok_Rpar; i += 1;
        break;
    case '*':
        switch( at(str,i+1) ){
        case '/':
            res = Tok_Rcmt; i += 2;
            break;
        case '=':
            res = Tok_StarEq; i += 2;
            break;
        default:
            res = Tok_Star; i += 1;
            break;
        }
        break;
    case '+':
        if( at(str,i+1) == '=' ){
            res = Tok_PlusEq; i += 2;
        } else {
            res = Tok_Plus; i += 1;
        }
        break;
    case ',':
        res = Tok_Comma; i += 1;
        break;
    case '-':
        if( at(str,i+1) == '=' ){
            res = Tok_MinusEq; i += 2;
        } else {
            res = Tok_Minus; i += 1;
        }
        break;
    case '.':
        res = Tok_Dot; i += 1;
        break;
    case '/':
        if( at(str,i+1) == '*' ){
            res = Tok_Lcmt; i += 2;
        } else {
            res = Tok_Slash; i += 1;
        }
        break;
    case ':':
        if( at(str,i+1) == '=' ){
            res = Tok_ColonEq; i += 2;
        } else {
            res = Tok_Colon; i += 1;
        }
        break;
    case ';':
        res = Tok_Semi; i += 1;
        break;
    case '<':
        if( at(str,i+1) == '=' ){
            res = Tok_Leq; i += 2;
        } else {
            res = Tok_Lt; i += 1;
        }
        break;
    case '=':
        if( at(str,i+1) == '=' ){
            res = Tok_2Eq; i += 2;
        } else {
            res = Tok_Eq; i += 1;
        }
        break;
    case '>':
        if( at(str,i+1) == '=' ){
            res = Tok_Geq; i += 2;
        } else {
            res = Tok_Gt; i += 1;
        }
        break;
    case '?':
        res = Tok_Qmark; i += 1;
        break;
    case '[':
        if( at(str,i+1) == ']' ){
            res = Tok_LbrackRbrack; i += 2;
        } else {
            res = Tok_Lbrack; i += 1;
        }
        break;
    case ']':
        res = Tok_Rbrack; i += 1;
        break;
    case '^':
        res = Tok_Hat; i += 1;
        break;
    case '`':
        res = Tok_60; i += 1;
        break;
    case 'b':
        if( at(str,i+1) == 'e' ){
            if( at(str,i+2) == 'g' ){
                if( at(str,i+3) == 'i' ){
                    if( at(str,i+4) == 'n' ){
                        res = Tok_begin; i += 5;
                    }
                }
            }
        }
        break;
    case 'c':
        if( at(str,i+1) == 'l' ){
            if( at(str,i+2) == 'a' ){
                if( at(str,i+3) == 's' ){
                    if( at(str,i+4) == 's' ){
                        res = Tok_class; i += 5;
                    }
                }
            }
        }
        break;
    case 'd':
        if( at(str,i+1) == 'e' ){
            if( at(str,i+2) == 'f' ){
                if( at(str,i+3) == 'i' ){
                    if( at(str,i+4) == 'n' ){
                        if( at(str,i+5) == 'e' ){
                            res = Tok_define; i += 6;
                        }
                    }
                }
            }
        }
        break;
    case 'e':
        switch( at(str,i+1) ){
        case 'l':
            if( at(str,i+2) == 's' ){
                switch( at(str,i+3) ){
                case 'e':
                    res = Tok_else; i += 4;
                    break;
                case 'i':
                    if( at(str,i+4) == 'f' ){
                        res = Tok_elsif; i += 5;
                    }
                    break;
                }
            }
            break;
        case 'n':
            if( at(str,i+2) == 'd' ){
                res = Tok_end; i += 3;
            }
            break;
        }
        break;
    case 'f':
        if( at(str,i+1) == 'a' ){
            if( at(str,i+2) == 'l' ){
                if( at(str,i+3) == 's' ){
                    if( at(str,i+4) == 'e' ){
                        res = Tok_false; i += 5;
                    }
                }
            }
        }
        break;
    case 'i':
        switch( at(str,i+1) ){
        case 'f':
            res = Tok_if; i += 2;
            break;
        case 'm':
            if( at(str,i+2) == 'p' ){
                if( at(str,i+3) == 'o' ){
                    if( at(str,i+4) == 'r' ){
                        if( at(str,i+5) == 't' ){
                            res = Tok_import; i += 6;
                        }
                    }
                }
            }
            break;
        case 'n':
            if( at(str,i+2) == 'c' ){
                if( at(str,i+3) == 'l' ){
                    if( at(str,i+4) == 'u' ){
                        if( at(str,i+5) == 'd' ){
                            if( at(str,i+6) == 'e' ){
                                res = Tok_include; i += 7;
                            }
                        }
                    }
                }
            } else {
                res = Tok_in; i += 2;
            }
            break;
        }
        break;
    case 'l':
        if( at(str,i+1) == 'e' ){
            if( at(str,i+2) == 't' ){
                res = Tok_let; i += 3;
            }
        }
        break;
    case 'p':
        if( at(str,i+1) == 'a' ){
            if( at(str,i+2) == 'r' ){
                if( at(str,i+3) == 'a' ){
                    if( at(str,i+4) == 'm' ){
                        res = Tok_param; i += 5;
                    }
                }
            }
        }
        break;
    case 's':
        if( at(str,i+1) == 'u' ){
            if( at(str,i+2) == 'b' ){
                if( at(str,i+3) == 'd' ){
                    if( at(str,i+4) == 'i' ){
                        if( at(str,i+5) == 'r' ){
                            res = Tok_subdir; i += 6;
                        }
                    }
                }
            }
        }
        break;
    case 't':
        switch( at(str,i+1) ){
        case 'h':
            if( at(str,i+2) == 'e' ){
                if( at(str,i+3) == 'n' ){
                    res = Tok_then; i += 4;
                }
            }
            break;
        case 'r':
            if( at(str,i+2) == 'u' ){
                if( at(str,i+3) == 'e' ){
                    res = Tok_true; i += 4;
                }
            }
            break;
        case 'y':
            if( at(str,i+2) == 'p' ){
                if( at(str,i+3) == 'e' ){
                    res = Tok_type; i += 4;
                }
            }
            break;
        }
        break;
    case 'v':
        if( at(str,i+1) == 'a' ){
            if( at(str,i+2) == 'r' ){
                res = Tok_var; i += 3;
            }
        }
        break;
    case '{':
        res = Tok_Lbrace; i += 1;
        break;
    case '|':
        if( at(str,i+1) == '|' ){
            res = Tok_2Bar; i += 2;
        }
        break;
    case '}':
        res = Tok_Rbrace; i += 1;
        break;
    }
    if(pos) *pos = i;
    return res;
}


char*bslex_allocstr(BSHiLex* l, int len)
{
    BSHiLexMemSlot* mem = (BSHiLexMemSlot*)malloc(sizeof(BSHiLexMemSlot)+ len - sizeof(void*));
    if( mem == 0 )
        return 0; // delegate error handling to caller
    mem->next = l->mem;
    l->mem = mem;
    return mem->str;
}
