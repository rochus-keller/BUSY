--[[
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
]]--

-- TODO: sync with bsparser.h BSNodeType, BSBaseType and BSVisibility
local BaseType, ListType, ModuleDef, ClassDecl, EnumDecl, VarDecl, FieldDecl, BlockDef, ProcDef = 1, 2, 3, 4, 5, 6, 7, 8, 9
local boolean, integer, real, string, path, symbol = 1, 2, 3, 4, 5, 6
local private, public, protected = 0, 1, 2


local globals = {}
local inst = {}
globals["#inst"] = inst
setmetatable(inst,globals)

local function basetype(name,type)
	local t = { ["#kind"] = BaseType, ["#type"] = type, ["#name"] = name, ["#visi"] = public, ["#owner"] = globals }
	globals[name] = t
end

-- base types
basetype("bool",boolean)
basetype("int",integer)
basetype("real",real)
basetype("string",string)
basetype("path",path)
basetype("symbol",symbol)

-- standard enums
local curEnum

local function enumtype(name)
	local c = { ["#kind"] = EnumDecl, ["#name"] = name, ["#owner"] = globals }
	globals[name] = c
	curEnum = c
end

local function enumitem(name)
	local n = curEnum["#n"]
	if n == nil then n = 1 else n = n + 1 end
	if n == 1 then curEnum["#default"] = name end
	curEnum[name] = n
	curEnum["#n"] = n
end

enumtype("LibraryType")
	enumitem("static")
	enumitem("shared")
	enumitem("framework")
	
enumtype("MessageType")
	enumitem("error")
	enumitem("warning")
	enumitem("info")
	
enumtype("OsType") 
	enumitem("darwin")
	enumitem("macos")
	enumitem("win32")
	enumitem("winrt")
	enumitem("cygwin")
	enumitem("solaris")
	enumitem("linux")
	enumitem("freebsd")
	enumitem("netbsd")
	enumitem("openbsd")
	enumitem("unix")

enumtype("CpuType")
	enumitem("arm")
	enumitem("x86")
	enumitem("ia64")
	enumitem("mips")
	enumitem("ppc")
	enumitem("s390")
	enumitem("sparc")

enumtype("CompilerType")
	enumitem("msvc")
	--enumitem("bor")
	enumitem("gcc")
	--enumitem("intel")
	enumitem("clang")
	
enumtype("WordSize")
	enumitem("16")
	enumitem("32")
	enumitem("64")
	enumitem("128")

enumtype("FileType")
	enumitem("object_file")
	enumitem("source_file")
	enumitem("static_lib")
	enumitem("shared_lib")
	enumitem("executable")


-- standard classes
local curClass

local function class(name,super)
	local c = { ["#kind"] = ClassDecl, ["#name"] = name, ["#visi"] = public, ["#super"] = super, ["#owner"] = globals }
	globals[name] = c
	if super ~= nil then
		for i = 1, #super do
			local sf = super[i]
			c[i] = sf
			c[sf["#name"]] = sf
		end
	end
	curClass = c
end

local function field(name, type)
	local c = { ["#kind"] = FieldDecl, ["#name"] = name, ["#visi"] = public, ["#owner"] = curClass, ["#type"] = type }
	curClass[name] = c
	curClass[ #curClass + 1 ] = c
end

local function listOf(type)
	local list = { ["#kind"] = ListType, ["#type"] = type }
	return list
end

class("Product")
	field("deps", listOf(globals.Product))
	--field("data_deps", listOf(globals.Product))
	--field("public_deps", listOf(globals.Product))

class("Config")
	field("cflags", listOf(globals.string))
	field("cflags_c", listOf(globals.string))
	field("cflags_cc", listOf(globals.string))
	field("cflags_objc", listOf(globals.string))
	field("cflags_objcc", listOf(globals.string))
	field("include_dirs", listOf(globals.path))
	field("defines", listOf(globals.string))
	--field("inputs", listOf(globals.Product))
	field("ldflags", listOf(globals.string))
	field("lib_dirs", listOf(globals.path))
	field("lib_names", listOf(globals.string))
	field("frameworks", listOf(globals.string))
	field("lib_files", listOf(globals.path))
	field("configs", listOf(globals.Config))
	
class("ConfigurableProduct", globals.Product)
	--field("all_dependent_configs", listOf(globals.Config))
	--field("public_configs", listOf(globals.Config))

class("CompiledProduct", globals.ConfigurableProduct )
	field("cflags", listOf(globals.string))
	field("cflags_c", listOf(globals.string))
	field("cflags_cc", listOf(globals.string))
	field("cflags_objc", listOf(globals.string))
	field("cflags_objcc", listOf(globals.string))
	field("sources", listOf(globals.path))
	field("include_dirs", listOf(globals.path))
	field("defines", listOf(globals.string))
	--field("inputs", listOf(globals.Product))
	field("ldflags", listOf(globals.string))
	field("lib_dirs", listOf(globals.path))
	field("lib_names", listOf(globals.string))
	field("frameworks", listOf(globals.string))
	field("lib_files", listOf(globals.path))
	field("configs", listOf(globals.Config))
	--field("data", listOf(globals.path))

class("Executable", globals.CompiledProduct )
	field("name", globals.string) 

class("Library", globals.CompiledProduct)
	field("lib_type", globals.LibraryType) 
	field("def_file", globals.path) 
	field("name", globals.string) 

class("SourceSet", globals.CompiledProduct )

class("Group",globals.Product) -- TODO globals.ConfigurableProduct

class("Action", globals.Product)

class("Script", globals.Action)
	field("args", listOf(globals.string))
	--field("inputs", listOf(globals.Product))
	field("script", globals.path)
	--field("data", listOf(globals.path))

class("LuaScript", globals.Script)

class("LuaScriptForeach", globals.Script)
	field("sources", listOf(globals.path))
	
class("Copy", globals.Action)
	field("sources", listOf(globals.path)) 
	field("outputs", listOf(globals.path))
	field("use_deps", listOf(globals.FileType))

class("Message", globals.Action)
	field("msg_type", globals.MessageType) 
	field("text", globals.string)

class("Moc", globals.Action)
	field("sources", listOf(globals.path))
	field("defines", listOf(globals.string))
	field("tool_dir", globals.path)

class("Rcc", globals.Action)
	field("sources", listOf(globals.path))
	field("tool_dir", globals.path)

---- predeclared procs

local function proc(name,id)
	local p = { ["#kind"] = ProcDef, ["#name"] = name, ["#visi"] = public, ["#id"] = id, ["#owner"] = globals }
	globals[name] = p
end

proc("samelist",1)
proc("sameset",2)
proc("toint",3)
proc("toreal",4)
proc("tostring",5)
proc("topath",6)
proc("error",8)
proc("warning",9)
proc("message",10)
proc("dump",11)
proc("abspath",12)
proc("relpath",13)
proc("readstring",14)
proc("trycompile",15)
proc("build_dir",16)
proc("modname",17)
proc("set_defaults",18)

---- built-in vars
local function variable(name, type, ro)
	local c = { ["#kind"] = VarDecl, ["#name"] = name, ["#visi"] = public, ["#type"] = type, ["#ro"] = ro, ["#owner"] = globals }
	globals[name] = c
end

variable("busy_version", globals.string, true)
variable("host_cpu", globals.CpuType, true)
variable("host_cpu_ver", globals.int, true)
variable("host_wordsize", globals.WordSize, true )
variable("host_os", globals.OsType, true)
variable("host_toolchain", globals.CompilerType, true)
variable("host_toolchain_ver", globals.int, true)
variable("root_build_dir", globals.path, true)
variable("root_source_dir", globals.path, true)
variable("target_cpu", globals.CpuType, false)
variable("target_cpu_ver", globals.int, false)
variable("target_wordsize", globals.WordSize, false)
variable("target_os", globals.OsType, false)
variable("target_toolchain", globals.CompilerType, false)
variable("target_toolchain_ver", globals.int, false)
variable("moc_path", globals.path, false)
variable("rcc_path", globals.path, false)

B = require("BUSY")
-- preset global variables; inst is the instance of the globals declaration
inst.busy_version = B.version()
inst.host_cpu, inst.host_cpu_ver = B.cpu()
inst.target_cpu, inst.target_cpu_ver = inst.host_cpu, inst.host_cpu_ver
inst.host_wordsize = tostring(B.wordsize() * 8)
inst.target_wordsize = inst.host_wordsize
inst.host_os = B.os()
inst.target_os = inst.host_os
inst.host_toolchain, inst.host_toolchain_ver = B.compiler()
inst.target_toolchain, inst.target_toolchain_ver = inst.host_toolchain, inst.host_toolchain_ver
inst.moc_path = "."
inst.rcc_path = "."
inst["#ctdefaults"] = {} -- a table with optional entries CompilerType->Config
inst["#ctdefaults"]["gcc"] = { ["cflags"] = { "-O2" } }
inst["#ctdefaults"]["clang"] = inst["#ctdefaults"]["gcc"]
inst["#ctdefaults"]["msvc"] = { ["cflags"] = { "/O2", "/MD" } }


return globals
