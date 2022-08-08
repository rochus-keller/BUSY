#ifndef BSUNICODE_H
#define BSUNICODE_H

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

#define UNICODE_LAST_CODEPOINT 0x10ffff

typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned int uint;

typedef struct UnicodeProperties {
    ushort category         : 8; /* 5 needed */
    ushort line_break_class : 8; /* 6 needed */
    ushort direction        : 8; /* 5 needed */
    ushort combiningClass   : 8;
    ushort joining          : 2;
    signed short digitValue : 6; /* 5 needed */
    ushort unicodeVersion   : 4;
    ushort lowerCaseSpecial : 1;
    ushort upperCaseSpecial : 1;
    ushort titleCaseSpecial : 1;
    ushort caseFoldSpecial  : 1; /* currently unused */
    signed short mirrorDiff    : 16;
    signed short lowerCaseDiff : 16;
    signed short upperCaseDiff : 16;
    signed short titleCaseDiff : 16;
    signed short caseFoldDiff  : 16;
    ushort graphemeBreak    : 8; /* 4 needed */
    ushort wordBreak        : 8; /* 4 needed */
    ushort sentenceBreak    : 8; /* 4 needed */
} UnicodeProperties;

extern const UnicodeProperties * unicode_properties_ucs4(uint ucs4);
extern const UnicodeProperties * unicode_properties_ucs2(ushort ucs2);
extern uint unicode_decode_utf8(const uchar* in, uchar* len );
extern int unicode_isletter(uint ucs4);
extern int unicode_isdigit(uint ucs4);
extern int unicode_isspace(uint ucs4);
extern int unicode_ispunctuation(uint ucs4);

enum Category
{
    // see https://www.compart.com/en/unicode/category
    NoCategory,

    Mark_NonSpacing,          //   Mn
    Mark_SpacingCombining,    //   Mc
    Mark_Enclosing,           //   Me

    Number_DecimalDigit,      //   Nd
    Number_Letter,            //   Nl
    Number_Other,             //   No

    Separator_Space,          //   Zs
    Separator_Line,           //   Zl
    Separator_Paragraph,      //   Zp

    Other_Control,            //   Cc
    Other_Format,             //   Cf
    Other_Surrogate,          //   Cs
    Other_PrivateUse,         //   Co
    Other_NotAssigned,        //   Cn

    Letter_Uppercase,         //   Lu
    Letter_Lowercase,         //   Ll
    Letter_Titlecase,         //   Lt
    Letter_Modifier,          //   Lm
    Letter_Other,             //   Lo

    Punctuation_Connector,    //   Pc
    Punctuation_Dash,         //   Pd
    Punctuation_Open,         //   Ps
    Punctuation_Close,        //   Pe
    Punctuation_InitialQuote, //   Pi
    Punctuation_FinalQuote,   //   Pf
    Punctuation_Other,        //   Po

    Symbol_Math,              //   Sm
    Symbol_Currency,          //   Sc
    Symbol_Modifier,          //   Sk
    Symbol_Other,             //   So

    Punctuation_Dask = Punctuation_Dash
};

#endif 
