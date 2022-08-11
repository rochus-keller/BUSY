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

B = require "BUSY"
S = require "string"

print("# Hello from BUSY version "..B.version())

local pathToSource, pathToBuild
local params = {}
local products
local checkOnly = false

local function parseParam(str)
	if str == "" or str == nil then error("option -P expects a string of the form 'key=value' or 'key' (where '=true' is implicit)") end
	local i = S.find(str,"=")
	if i == nil then
		if params[str] then error("parameter already set: "..tostring(str)) end
		params[str] = true
	else
		local key = S.sub(str,1,i-1)
		local val = S.sub(str,i+1)
		if key == "" then error("invalid key in -P "..tostring(str)) end
		if val == "" then error("invalid value in -P "..tostring(str)) end
		if params[key] then error("parameter already set: "..tostring(key)) end
		params[key] = val
	end
end

local i = 1
while i <= #arg do
	if arg[i] == "-B" then
		i = i + 1
		-- explicitly set <path-to-build>, default is './output'
		if pathToBuild ~= nil then error("<path-to-build> already set") end
		pathToBuild = arg[i]
	elseif arg[i] == "-S" then
		i = i + 1
		-- explicitly <path-to-source>, set default is '..'
		if pathToSource ~= nil then error("<path-to-source> already set") end
		pathToSource = arg[i]
	elseif arg[i] == "-T" then
		i = i + 1
		-- explicitly select one or more of the public product instances (targets) to build; e.g. '-T a.b'
		-- if no products are explicitly selected, the default products (visibility '!') of the root are selected
		if products == nil then products = {} end
		if products[arg[i]] ~= nil then error("target already selected: "..tostring(arg[i])) end
		products[arg[i]] = true
	elseif arg[i] == "-P" then
		i = i + 1
		-- passing values to parameters (either root or visible subdirs); e.g. '-P x.y=value' like cmake
		parseParam(arg[i])
	-- elseif arg[i] == "-G" then
		-- TODO optionally specify generator; default immediately builds products; e.g. like cmake '-G ninja'
	elseif arg[i] == "-c" then
		checkOnly = true
	elseif arg[i]:sub(1,1) == "-" then
		error("unknown option "..arg[i])
	else
		-- simple call similar to cmake: <path-to-source>
		-- cmake call with <path-to-existing-build> is not supported
		if pathToSource ~= nil then error("<path-to-source> already set") end
		pathToSource = arg[i] -- i.e. an option with no -X prefix is interpreted as the path to the source tree root
	end
	i = i + 1
end


local root = B.compile(pathToSource,pathToBuild,params)

if not checkOnly then
	B.execute(root,products)
end


