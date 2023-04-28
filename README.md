# DynASM demo

An example of a practical use of DynASM, the dynamic assembler from the LuaJIT
project by Mike Pall:

 - https://luajit.org/dynasm.html

An indespensable resource for use of DynASM, except for the official sources is
the unofficial documentation by Peter Cawley, which includes a tutorial and a
reference for the DynASM API and the x86/x86-64 instructions:

 - https://corsix.github.io/dynasm-doc/

A great blog post introducing JITs and featuring DynASM has been written by
Josh Haberman:

 - https://blog.reverberate.org/2012/12/hello-jit-world-joy-of-simple-jits.html

This example demonstrates a template JIT compilation of a program for a very
simple stack machine. The bytecode design, an example program and the
original interpreter implemenation is due to Martin Dørum:

 - https://mort.coffee/home/fast-interpreters/

The demo features:

 - `dynasm` directory: subset of DynASM for the x86-64 architecture.  By Mike
   Pall (MIT license).

 - `dynasm/minilua.c`: minified, single file PUC Lua 5.1 from PUC-Rio (MIT
   license) with bit operation extensions by Mike Pall.

 - `src/demo.c`: single file demo showing the use of DynASM. Heavily based on
   Peter Cawley's unofficiall DynASM documentation (CC BY 3.0) and on the code
   from Martin Dørum's blog post (shamefully stolen).

 - `meson.build`: a build for for the [Meson](https://mesonbuild.com/) build
   system. It compiles `minilua.c` into a Lua interpreter, runs with it
   `dynasm/dynasm.lua` Lua script, which preprocesses the `src/demo.c` C file
   into code with calls to DynASM C API. The DynASM C runtime is compiled
   directly into `src/demo.c` through includes of `dynasm/dasm_proto.h` and
   `dynasm/dasm_x86.h`.

The inclusion of `minilua` makes the project self contained---it needs just a C
compiler (and Meson). If your Lua interpretere supports bit operations, you can
use it as well (in particular `luajit` works).

The `src/demo.c` is thoroughly commented. It can be overwhelming though and the
example is not very realistic. Several iterations are made on top, to show
possible improvements. These are currently without comments and are contained
in the following `git` branches:

 - `master` - thoroughly documented compiliation of bytecode for a stack
   machine, which uses x86-64 stack as _the stack_.

 - `part1` - same as `master`, but without comments.

 - `part2` - a custom stack is allocated and custom stack pointer is managed.

 - `part3` - DynASM "type maps" (`.type` directives) are used to improve the
   readability of the assembly.

 - `part4` - a state struct is introduced, which is used to hold DynASM state
   as well as other state for a possible interpreter, which JITs some bytecodes
   and interpreters others. The example is too simplistic to show anything
   real, but at least shows how to integrate custom state `struct` with DynASM
   and how to move state from it to registers.

Some other JIT/x86-64 resources I found useful:

x86-64 basics and ABI:

 - [NASM Tutorial](https://cs.lmu.edu/~ray/notes/nasmtutorial/)
 - [Eli Bendersky: Stack frame layout on x86-64](https://eli.thegreenplace.net/2011/09/06/stack-frame-layout-on-x86-64)
 - [Eli Bendersky: Where the top of the stack is on x86](https://eli.thegreenplace.net/2011/02/04/where-the-top-of-the-stack-is-on-x86/)
 - [x86-64 System V ABI](https://gitlab.com/x86-psABIs/x86-64-ABI)
 - [x86-64 Microsoft ABI](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)

`int3` and debugging:

 - [Eli Bendersky: How debuggers work part 2](https://eli.thegreenplace.net/2011/01/27/how-debuggers-work-part-2-breakpoints)


Issues with relative offsets, global variables, linking (or lack thereof in case of JITed code):

 - [Eli Bendersky: Position Independent Code (PIC) in shared libraries](https://eli.thegreenplace.net/2011/11/03/position-independent-code-pic-in-shared-libraries)
 - [Eli Bendersky: Load-time relocation of shared libraries](https://eli.thegreenplace.net/2011/08/25/load-time-relocation-of-shared-libraries)
 - [Stack Overflow: Handling calls to (potentially) far away ahead-of-time compiled functions from JITed code](https://stackoverflow.com/questions/54947302/handling-calls-to-potentially-far-away-ahead-of-time-compiled-functions-from-j)
 - [Stack Overflow: How to load address of function or label into register?](https://stackoverflow.com/questions/57212012/how-to-load-address-of-function-or-label-into-register)
 - [Stack Overflow: Call an absolute pointer in x86 machine code](https://stackoverflow.com/questions/19552158/call-an-absolute-pointer-in-x86-machine-code)

Debugging issues:

 - [Stack Overflow: How to use gdb stacktrace with run time generated machine code?](https://stackoverflow.com/questions/34940738/how-to-use-gdb-stacktrace-with-run-time-generated-machine-code)

Starting with JITs:

 - [Eli Bendersky: How to JIT - an introduction](https://eli.thegreenplace.net/2013/11/05/how-to-jit-an-introduction)
 - [Chris Wellons: A Basic Just-In-Time Compiler](https://nullprogram.com/blog/2015/03/19/)
 - [Eli Bendersky: Adventures in JIT compilation: Part 1 - an interpreter](https://eli.thegreenplace.net/2017/adventures-in-jit-compilation-part-1-an-interpreter/)
 - [Eli Bendersky: Adventures in JIT compilation: Part 2 - an x64 JIT](https://eli.thegreenplace.net/2017/adventures-in-jit-compilation-part-2-an-x64-jit/)
 - [Spencer Tipping: How to write a JIT compiler](https://github.com/spencertipping/jit-tutorial)
