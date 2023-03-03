#ifndef BSVISITOR_H
#define BSVISITOR_H

/*
* Copyright 2023 Rochus Keller <mailto:me@rochus-keller.ch>
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
#include "bscallbacks.h"


typedef struct BSVisitorCtx {
    void* d_data;
    BSLogger d_log;
    BSBeginOp d_begin;
    BSOpParam d_param;
    BSEndOp d_end;
    BSForkGroup d_fork;
} BSVisitorCtx;

extern int bs_visit(lua_State* L);
// param: productinst
// param: BSVisitorCtx userdata



#endif // BSVISITOR_H
