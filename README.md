![icon](http://software.rochus-keller.ch/busy-logo-250x100.png)

## Welcome to the BUSY build system

BUSY (for *BU*ild *SY*stem) is a lean, cross-platform build system for the GCC, Clang and MSVC toolchains, with very little system requirements and easy bootsrapping.

Compared to other build systems like CMake, QMake, Meson or GN, BUSY is characterized by a **statically typed** build specification language, and by the possibility to build a project directly from scratch without any further requirements to the host system; BUSY is so lean that it is even suited to be directly integrated with the source tree of a project. 

Here is an **example project** using BUSY: https://github.com/rochus-keller/nappgui/. NAppGUI is an extensive cross-platform GUI library written in C89 (with parts in C++99 and Objective-C) by Francisco García. See the README on how to build the project and check the BUSY files in the root and the src directory (and subdirectories). See below and in the syntax directory for more information about the specification language. Those who know GN will recognize various concepts in BUSY.

Here are a few excerpts for convenience:

```
# from the top-level BUSY file
submod src

let shared_lib* = src.shared_lib
let static_lib* = src.static_lib
let all! : Group {
	.deps = [ shared_lib static_lib ]
	# since 'let' is used the 'deps' field can only be set here using the '.' prefix
	# if 'var' is used instead it could be modified elsewhere with 'all.deps += xyz'
}
```
```
# from the src BUSY file
let main_config - : Config {
	.include_dirs += [ ./geom2d ./osbs ./sewer /* just a few shown */ ]
	.defines += [ "NAPPGUI_LIBRARY" 
		"NAPPGUI_BUILD_DIR=\"" + tostring(root_build_dir) + "\""
		"NAPPGUI_BUILD=\"" + readstring('../prj/build.txt') + "\"" ]
}

submod core
submod draw2d
	# and many more
	
let all_lib_sources : Group { # only visible in this BUSY file
	.deps = [
		core.sources
		draw2d.sources
		# and many more
	]
}

let static_lib* : Library {
	.name = "NAppGUI" # otherwise the binary would be named "static_lib"
	.lib_type = `static
	.deps = [ all_lib_sources ]
}
```
```
# from the draw2d BUSY file
submod gtk3

let sources * : SourceSet {
	.sources = [
		./draw2d.cpp 
		./drawg.cpp 
		./btext.c 
		# and many more
	]
	.configs += ^main_config # this references the Config in the src BUSY file
	if target_os == `linux {
		.deps += gtk3.sources
	}else if target_os == `win32 {
		# and on and on
	}else {
		error("target os not supported")
	}
}
```
Also this syntax version is valid (for people who prefer the Pascal style):
```
let sources * : SourceSet 
begin
	.sources := [
		./draw2d.cpp 
		./drawg.cpp 
		./btext.c 
	]
	if target_os == `linux then
		.deps += gtk3.sources
	elsif target_os == `win32 then
		# and on and on
	else
		error("target os not supported")
	end
end
```

Another, a more complex example using BUSY is the [Oberon+ compiler and IDE](https://github.com/rochus-keller/Oberon); see https://github.com/rochus-keller/LeanQt/blob/main/Readme.md on how to run the build. It also demonstrates the special support of BUSY for the Qt moc and rcc tools. 

BUSY is based on and integrated with the Lua virtual machine (but it is written in C89, not in Lua). Lua is by far one of the easiest to build code bases on all platforms; the only requirement is a C89 compiler; BUSY follows this tradition and benefits from the great work of the Lua authors.

### Why yet another build system?

I'm using and studying build systems for many years. QMake is the build system I have worked with the most over the past twenty years; but it requires Qt, and it is not as flexible for large projects as e.g. GN. I also use CMake in a few projects, and I've been following its development since I first got involved with the Visualization Toolkit twenty years ago. And I track newer projects like Meson or GN; I came across the latter while exploring the Dart VM source tree, and I've even developed an analysis tool for it (see https://github.com/rochus-keller/GnTools). The findings would be enough material for several articles; here is just a brief overview.

CMake is a fully equiped, and unfortunately somewhat antiquated scripting language; "modern CMake" now also focuses on targets and properties (similar to GN) instead of imperative detail specifications of how the individual steps of a build should proceed; that's the right direction, but still just another layer that explores the limits of this string-typed dynamic scripting language, with all other layers still shining through; CMake itself has become a huge, complex system that a single developer hardly fully understands anymore; it's also bigger and more complex than most projects I usually want to build with it.

Meson has brought interesting approaches and explicitly avoids a Turing complete programming language, which is the right way from my point of view; I also like the idea to access results via references to abstract objects and thus to abstract away file system paths; unfortunately, however, the language is somewhat peculiar, apparently inspired by Python, and still dynamically typed (even if there are more than just string types).

Build systems have always seemed to be something that software developers want to spend as little time on as possible; they seem to be considered a by-product, not in the focus of the actual art of software engineering; only with this I can explain myself, why the achievements of the software engineering of the past fifty years, like modularization, structured and object-oriented programming, and static type checking had apparently so little influence. This is taking its toll now that software systems are getting bigger and platform independence is a must. Chromium is an impressive example and has grown over the years into an incredibly large system, now with close to 40 million lines of code; the developers have introduced and discarded several build systems; since a few years GN is being used, which was even developed specifically for Chromium's needs; GN is very powerful, but also rather complex; even understanding the build process of the Dart VM, which is much smaller than Chromium, is far from easy; while [my tool](https://github.com/rochus-keller/GnTools) has been helpful in this regard, it inherently reaches the limits of what a dynamically typed language can offer in terms of static analysis; thus, developing, analyzing and maintaining a build system for a large software system suffers from essentially the same drawbacks known from dynamic languages, and which ultimately led to developments such as TypeScript or Dart; in my view, it has long been necessary for build systems to have static typing and effective modularization means as well.

But there are other aspects that, in my view, have not been taken into account enough by previous build systems. First of all, the question must be raised, how a build system itself is actually built and how expensive and complicated this may be; the requirements that a build system makes on the existing system and pre-installed components should be minimal; GN, Meson or QMake come off rather weakly in this respect, because a compiler and rather big libraries for a recent version of C++ and/or Python are required; I once created a stand-alone version of QMake with a minimal, stripped-down set of Qt classes; but it still consists of nearly 200 source code files, and trying to compile it with something like `g++ *.cpp` eats up all available memory and crashes within seconds; GN and Meson both also require Ninja which is yet another C++ code base which has to be built. 

So how about something like Lua which only requires a C89 compatible compiler, and which generates a single binary of a few hundered kilobytes? And `gcc *.c` works perfectly well with Lua. Also writing a parser for a statically typed language in C89 is feasible. 

This led me to try my luck myself and throw my hat into the ring. 

### Running BUSY

BUSY is usually started using the build.lua script, followed by additional arguments; build.lua is just a façade to the C implemented Lua functions which makes it possible to extend or change the build system using Lua scripts if need be.

BUSY by default uses the toolchain which was used to build the BUSY executable. 

The default command line version `lua build.lua` does the following: The root of the source tree is assumed to be at `..`, and root of the build directory tree is assumed at `./output`. Therefore the easiest way to run BUSY is to create a build subdirectory on the top-level of the source tree and put and compile the BUSY source code there; but you don't have to (see the other options below). By default all products in the root BUSY file are built (i.e. the build is directly run) which are marked as default (by putting a `!` after their name). 

With the `-S` option you can explicitly set the path to the root of the source directory tree; as a shortcut, you can also just type the path without the `-S` prefix.

With the `-B` option you can explicitly set the path to the root of the build directory tree.

With the `-T` option you can explicitly select which products should be built; e.g. `-T my_lib` would look for a public variable declaration of a `Product` subtype named "my_lib" in the root BUSY file; it is also possible to select more than one product, or a product from a BUSY file located further down the source tree; the latter must be visible from the root (i.e. the variable and submodule declarations in the designator must be public).

With the `-P` option parameter values can be set; the syntax is `-P x.y=value`, where value is a valid BUSY basic type literal syntax; the syntax of strings and symbols usually has to make use of command line escapes, e.g. like `-P "string_param=\"this is a string\""`, or ``-P symbol_param=\`abc``. Again it is possible to set parameters of BUSY files located further down the source tree, but only if the submodule declarations in the designator are public.

With the `-M` option one of the build modes `optimized`, `nonoptimized` or `debug`can be set; e.g. `-M debug`; the default build mode is `optimized`. BUSY also supports the abbreviated options `-opt` (for `optimized`), `-nopt` (for `nonoptimized`) or `-dgb` (for `debug`). 

With the `-c` option only the parser/analyzer is run to check the BUSY files. No build is run, no files or directories are generated.

With the `-G` option you can tell BUSY to generate code for another build system. Currently the option `-G qmake` is supported to generate the project files required to use QtCreator with the project. In a future version of BUSY, other backends like `-G ninja` will be supported. If no `-G` option is provided, BUSY just runs the build itself.

### Specifying builds

Builds are specified using BUSY files including code written in the BUSY specification language; see [The BUSY Build System - Language and Built-ins Specification](http://software.rochus-keller.ch/busy_spec.html) for detailed information about the specification language.

A BUSY file is a file with the filename "BUSY", or alternatively "BUSY.busy"; if both files are present in a directory, the file named "BUSY" is given priority.

There is a BUSY file in the root of the source tree and any subdirectory which includes files or other subdirectories relevant to the build.

The BUSY files are the "modules" of the specification language. Declarations are only visible within the module unless declared public (`*`, visible to outer and nested modules) or protected (`-`, visible only to nested modules). Submodules must be explicitly declared and associated with the corresponding directories using the `submod` keyword. 

The specification makes use of pre-declared types, procedures and variables. Pre-declared types are the basic types `bool`, `int`, `real`, `string`, `path` and `symbol`, enumeration types like ```type LibraryType* = (`static, `shared, `framework)```, and class types like 

```
type Config* = class { 
		cflags : string[]
		defines: string[]
		include_dirs: path[]
		/* and so on */
		configs: Config[] 
	}
``` 

There is also a class hierarchy; e.g. the classes `Executable`, `Library` and `SourceSet` are all subclasses of `Product`, which has a field `deps: Product[]` to represent build time dependencies between products (i.e. targets). Note that this is all regular syntax which can be used in any BUSY file; there are no magic classes (like e.g. in Meson) which are somehow defined behind the curtains in another language. 

Pre-declared, global variables like `let root_build_dir: path`, `let host_os: OsType`, `let host_toolchain: CompilerType` or `let host_toolchain_ver: int` are set by BUSY and can be used to adapt your build to different operating systems or toolchains. Pre-declared "procedures" (they are actually not real procedures, but just hints to the compiler) like `tostring()` can be used to do type conversions, since e.g. assigning a number directly to a string variable gives a compiler error.

BUSY conceptually runs in three phases (similar to BAZEL): the loading phase (parses all BUSY files), the analysis phase (runs the statements and creates the work trees), and the execution phase (runs the selected work tree depth-first). It is important to note that even if all declarations are considered and different work trees are created, only the ones selected as default (identifier marked with `!`) or by the `-T` option are actually executed.


### Planned or work-in-progress features

The current BUSY version is feature complete for the use with [LeanQt](https://github.com/rochus-keller/leanqt), the [Oberon+ OBXMC tool](https://github.com/rochus-keller/Oberon/) and the [NAppGUI framework](https://github.com/rochus-keller/nappgui), and successfully tested on Linux x86, x86_64 and ARMv7 (GCC), Windows 10 x86 and AMD64 (MSVC), Windows 7 x86 (MSVC), and macOS 10.11 x86_64 and 12.2 M1 (CLANG). 

- [x] Statically typed build specification language, "as simple as possible"
- [x] Support for unicode (UTF-8) in strings, symbols, paths, comments and identifiers
- [x] Support C as well as Pascal flair syntax versions
- [x] Able to directly run the build (i.e. independently of Make, NMake or Ninja)
- [x] Language specification
- [x] Make a lean Qt source tree version using BUSY (see [LeanQt](https://github.com/rochus-keller/LeanQt))
- [x] QMake backend (tested with LeanQt on Linux, Windows and Mac with QtCreator 3 and 4)
- [ ] Support cross-compilation (work in progress, see NOTE)
- [ ] Tutorials
- [ ] Make a Mono CLR source tree version using BUSY
- [ ] Implement Ninja backend
- [ ] Implement CMake backend

NOTE:

Linux cross-compilation from x86 to ARM Cortex-A7 (Allwinner H3) successfully worked with this command:

```
lua build.lua ../LeanQt -P target_toolchain_path=//home/me/toolchain/bin -P HAVE_OBJECT -P target_toolchain_prefix=\"arm-linux-gnueabihf-\"
```


### Non-Goals

- BUSY is not and doesn't want to be a full programming language
- BUSY is no package generator or manager
- BUSY is not a Git client
- BUSY doesn't search for libraries or toolchains and doesn't download anything; it uses what you give it
- BUSY is not a C preprocessor and doesn't check #include directives
- BUSY is no test framework, generator or manager; though it can run external scripts
- BUSY is no replacement for Ninja; it can directly run builds to make it easier to deploy buildable code bases; for fast edit-compile-run cycles Ninja files can be generated

### Build Steps

Building BUSY is really simple:

1) Open a terminal and set current directory to the BUSY source code directory
2) Run `cc *.c -O2 -lm -o lua` or `cl /O2 /MD /Fe:lua.exe *.c` depending on whether you are on a Unix or Windows machine
3) Wait a few seconds; the result is a Lua executable with BUSY integrated.

## Additional Credits

BUSY uses the Lua 5.1.5 virtual machine made available under an MIT license by Luiz Henrique de Figueiredo, Roberto Ierusalimschy and Waldemar Celes at  PUC-Rio, Brazil; see http://www.lua.org.

BUSY uses some unicode tables and the os/cpu/toolchain detection logic derived from the Qt source code, which is made available to the public under the LGPL or GPL license; see https://www.qt.io

BUSY makes use of concepts which are implemented and well documented in the GN meta build system by Brett Wilson and colleagues, which is made available under a BSD-style license; see https://gn.googlesource.com/gn/

## Support

If you need support or would like to post issues or feature requests please use the Github issue list at https://github.com/rochus-keller/BUSY/issues or send an email to the author.



