// Michal Vlasák, FIT CTU, 2023
//
// An example of a practical use of DynASM, the dynamic assembler from the
// LuaJIT project by Mike Pall:
//
//         https://luajit.org/dynasm.html
//
// An indespensable resource for use of DynASM, except for the official sources
// is the unofficial documentation by Peter Cawley and includes a tutorial and
// a reference for the DynASM API and the x86/x86-64 instructions:
//
//         https://corsix.github.io/dynasm-doc/
//
// A great blog post introducing JITs and featuring DynASM has been written by
// Josh Haberman:
//
//         https://blog.reverberate.org/2012/12/hello-jit-world-joy-of-simple-jits.html
//
//
// This example demonstrates a template JIT compilation of a program for a very
// simple stack machine. The bytecode design, an example program and the
// original interpreter implemenation is due to Martin Dørum:
//
//         https://mort.coffee/home/fast-interpreters/

// Because of glibc versions before 2.37 by mistake didn't expose
// `MAP_ANONYMOUS` by default, and needed at least _BSD_SOURCE. But since 2.20,
// _BSD_SOURCE is deprecated and _DEFAULT_SOURCE should be used instead. Not
// ideal. See also feature_test_macros(7).
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

// First, some generic includes.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

// These defines are very not not portable. But the rest of our program depends
// on bigger details of the x86-64 architecture anyways, so assuming that int is
// 32 bit signed integer (which dynasm also assumes) is least of our concerns.
typedef unsigned char u8;
typedef int i32;
typedef unsigned int u32;

// We need mmap and mprotect (on POSIX systems) or VirtualAlloc and
// VirtualProtect (on Windows). See their later use in this file.
#if _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

// For Macs we need to use MAP_JIT, we wrap it in `MAP_JIT_VALUE`, so we can be
// platform independent in the rest of the code. Also on Macs we have to
// explicitly invalidate the instruction cache for the processor to see the new
// executable code. This is a noop on x86-64 (otherwise we would also need to
// consider the issue on other operating systems), but let's hope that just this
// is enough to work on ARM64 based Macs with Rosetta 2.
#if defined(__APPLE__) && defined(__MACH__)
#define MAP_JIT_VALUE MAP_JIT
void sys_icache_invalidate(void *start, size_t len);
#else
#define MAP_JIT_VALUE 0
#endif

// Before we include dynasm includes, we need to (or rather want to) define a
// few macros that these headers use. Skip ahead until you need to / want to
// understand this. They allow us to customize the signatures and bodies of all
// dynasm functions. Dynasm will translate assembly snippets in our C source by
// replacing them with calls to `dasm_put`, with `Dst` as the first argument,
// and then some other arguments we don't care about now.
//
//         dasm_put(Dst, index, varargs...)
//
// Dynasm functions have signatures and bodies which look something like this:
//
//         void
//         dasm_X(Dst_DECL, ...)
//         {
//                 Dst_REF = ...;
//                 ...
//                 dasm_State *D = Dst_REF;
//                 ...
//                 dasm_Y(Dst);
//                 ...
//         }
//
// I.e. two more macros (`Dst_DECL` and `Dst_REF`) are used. The macro
// `Dst_DECL` will become part of the function signature and `Dst_REF` needs to
// be able to evaluate to an lvalue of `dasm_State *`, since dynasm function may
// assign to it.
//
// 1) Our dynasm state will be local to only one function, so a local variable
// holding the `dasm_State *` is sufficient there.
//
// 2) Because the dynasm functions need lvalue of `dasm_State *` we will need a
// level of indirection. For convenience we will have another local variable
// which will hold `dasm_State **`, i.e. a double pointer, whose dereference
// evaluates to an lvalue of `dasm_State *`.
//
// 3) Since dasm functions also use `Dst` internally, `Dst`, `Dst_DECL` and
// `Dst_REF` will necessarily have to use the same variable name. We pick the
// name `ds` to snad for "DynASM state".
//
// 4) To ensure consistent use, all our functions needing `dasm_State` will also
// use `Dst_DECL` in their signature and `Dst` to refer to the state, i.e. for
// example to call other dynasm functions which expect the same parameter. The
// canonical DynASM functions will then be able to extract `dasm_State *` and
// operate on that structure.
//
// This seemingly complicated scheme introduced by DynASM has a nice property -
// the state passed to DynASM functions is controlled by us. Other macros allow
// more customization of e.g. allocation in DynASM and by providing our state,
// we can customize how DynASM allocates memory. Also in real system, we would
// have more state than `dasm_State *` anyways, so our use could look like this
// instead:
//
//         typedef struct {
//                 Heap heap;
//                 Program program;
//                 dasm_State *ds;
//         } State;
//
//         #define Dst       state
//         #define Dst_DECL  State *state
//         #define Dst_REF   (sate->ds)
//
#define Dst       ds
#define Dst_DECL  dasm_State **ds
#define Dst_REF   (*ds)

// Also last chance before we include the DynASM headers is to optionally enable
// some checks from DynASM. We'll have them enabled unconditionally, in practice
// we wouldn't have them enabled in a (sufficiently tested) release build.
#define DASM_CHECKS

// Now we can finally include the DynASM headers. The first one contains just
// the declarations of functions we can use or fallback definitions of macros,
// some of which we chose to override above. This header file is generic and
// applies to all dynasm backends regardless of architecture.
#include "dasm_proto.h"

// The second header file we include is specific for our architecture. Since we
// aim the x86-64 we need to include that. While the preprocessor has a separate
// file for the 64 bit version of x86, there is only one header common to both.
#include "dasm_x86.h"

// The `.arch` line tells the dynasm.lua prepreprocessor to load dasm_x64.lua,
// which knows the details of the x64 (x86-64) architecture. For this reason it
// has has to one of the first thing the preprocessor sees. But, this directive
// will also translate directly to some output: a check that the version of the
// preprocessor (dynasm.lua) is consistent with the header file (dasm_proto.h).
// So it can't be before the includes above.
//|.arch x64

// The above directive translates to something like this:
//
//         #if DASM_VERSION != 10500
//         #error "Version mismatch between DynASM and included encoding engine"
//         #endif

// Dynasm allows us to write snippets of assembly after `|` ("pipe"), or in our
// version after `//|`. These snippets are preprocessed into calls to the
// `dasm_put` function which appends the code to an internal dasm buffer, which
// after several passess (done by functions which will be introduced in due
// time), we can extract and have an executable series of bytes, i.e. code
// generated at runtime. The biggest devises of dynasm are:
//
//  1) We write the assembly snippets directly into our C source, which makes it
//     quite readable.
//
//  2) We can mix the assembly with regular C expressions. These expressions are
//     written statically in the source (i.e. `4 * offset`), but their values
//     can be different at different times `dynasm_put` is called (that is, when
//     the C execution gets to our code between `//|`). This is nice when we
//     have things with static _shapes_, but that can take advantage of having
//     different constants being embedded in the assembly. For an example a
//     bytecode `OP_GET_LOCAL index` instruction that sets the local at index
//     `index` to the value at the top of the stack, probably has an index
//     following it in the instruction stream. From the perspective of the
//     interpreter, the index has to be loaded everytime any `OP_GET_LOCAL`
//     instruction is executed, since different `GET_LOCAL` instructions can
//     have different indices. But if we were to translate that instruction into
//     assembly, we are always translating a concrete `GET_LOCAL` with a
//     concrete index, so what we want to generate generally will be the same
//     code, but with a different constant.
//
//     Suppose that locals are 8 bytes and that base pointer to locals is in
//     the register rbx, and that we want to push the value to the stack:
//
//   if (op == OP_GET_LOCAL) {
//           int index = READ_INDEX(ip + 1);
//           | mov rax, [rbx + (index * 8)]
//           | push rax
//   }
//
//   // Here `(index * 8)` is a regular C expression which will become the
//   argument to `dasm_put`, i.e. `dasm_put(..., (index * 8))`, other arguments
//   to `dasm_put` need to somehow encode the rest of the snippet, i.e. the
//   instruction bytes:
//
//           mov REG, [REG + CONST]
//           push REG
//
//
//
// `.actionlist` results in static array of unsigned chars (i.e. bytes), which
// constitute a bytecode for the runtime portion of dynasm which performs the
// puts, links and encodes at runtime. Different `dasm_put` calls use different
// portions of this buffer. There can be only one `.actionlist` per file.
//
// The bytecode contains bytes that are translated literally into the output,
// these are bytes that directly correspond to encodings of instructions.
//
// The "encoding time constants", i.e. the C expressions are not put into the
// action list. First of all, these are C expressions, that dynasm doesn't know
// much about (it is a simple text based preprocessor), but also naturally these
// expressions evaluate to different values at runtime. Instead, the bytecode
// contains also dynasm instructions, which tell dynasm that for example, now it
// should take the next expression (passed to `dasm_put` as argument) or that it
// is now encoding a forward jump with a 4 byte offset, which should be reserved
// in the first pass, but resolved in a subsequent pass.
//
//|.actionlist our_dasm_actions
//
// The above directive translates to something like this:
//
//
//         static const unsigned char our_dasm_actions[85] = {
//                 83,85,72,137,229,72,187,237,237,255,249,204,255,104,237,255,
//                 89,88,72,1,200, 80,255,72,184,237,237,72,191,237,237,94,252,
//                 ...
//         };
//
//  Generally most bytes correspond to instruction bytes directly, while the
//  high bytes (233+ at the moment) are DASM instructions.
//

// Labels are a well known to anybody who has seen any assembly code. They allow
// us to refer to positions in the code (or data, ...) by human readable names,
// instead of the relative offsets or absolute positions the machine code needs.
// DynASM allows us to have a fixed set of global labels, and give us an array
// of pointers (`void *`) to them. So not only can we refer to these labels in
// DynASM snippets and DynASM will translate the use of those labels to
// probably relative offsets of jumps / calls, but we will also have absolute
// addresses of these labels and can e.g. call them directly, by casting these
// pointers to function pointers and calling them.
//
// For example we can define an "identity" function that just returns its first
// (and only argument). According to the AMD 64 System V ABI, the first
// arguments is passed in register rdi and the return value is supposed to be in
// the register rax, so our function marked with the global "MY_IDENTITY" label
// looks like this:
//
//         |->MY_IDENTITY:
//         |
//         |  mov rax, rdi
//         |  ret
//
// We can refer this function from any other snippet and DynASM will translate
// the relative call offset as appropriate:
//
//         | mov rdi, 5
//         | call ->MY_IDENTITY
//
// The situation with offsets is not that easy though (they are only 32 bit,
// even on x86-64 where the address space is 64 bit), but we'll come back to
// that later.
//
// Or after processing it, we can access take the address of the function
// (`void *`), cast it appropriately and call it. The addresses of all globals
// are stored in a single globals array, which we need to allocate ourselves.
// The global functions are referred to by the means of their symbolic names
// through a C enum, with a prefix. We set this prefix by use of the `.globals`
// dynasm preprocessor directive, which will translate to the enum with all
// global label names (with the prefix).
//
//         void *our_dasm_labels[DASM_LBL__MAX];
//         dasm_setupglobal(Dst, our_dasm_labels, DASM_LBL__MAX);
//         // processing the snippet with the global label, as well as linking
//         // and encoding of the code are omitted, but after that we could do:
//         long (*identity)(long argument) = (long (*)(long)) dasm_labels[DASM_LBL_MY_IDENTITY];
//         long five = identity(5);
//
// Above, `identity` is a pointer to function taking one argument of the type
// `long` and returning a `long`. This is the type of the identity function we
// defined above (since `long` is 64 bit on x86-64). We cast the `void *` from
// the globals array and store it in `identity` so that we can then call it.
//
// (In standard C casting between data pointers and function pointers is not
// allowed, and indeed it doesn't work on some architectures, but we are fine
// on x86-64.)
//
// Here we tell the dynasm preprocessor what prefix we want to use for the enum,
// and it will also translate to that enum:
//
//|.globals DASM_LBL_
//
// The above translates to something like:
//         enum {
//                 DASM_LBL_MY_IDENTITY,
//                 DASM_LBL__MAX
//         };

// DynASM support multiple sectinons, though they are all currently limited to
// executable code, and "data" or "bss" sections are not supported. We will do
// with just one section for "code" we declare it here, which causes the
// preprocessor to define a couple of section related macros -- one for each
// section to give it an index, and one for the total number of sections that
// will came handy later, when we call `dasm_init` which needs to know the
// maximal number of sections we want to use.
//|.section code
//
// The above translates to something like:
//
//         #define DASM_SECTION_CODE   0
//         #define DASM_MAXSECTION     1

// In these days of virtual memory, most of the memory we can allocate won't be
// executable. So we need to allocate our own pages of memory, where we will put
// our code and make them executable. However, security-wise, having memory
// which is both writable and executable is far from ideal, so it is better to
// allocate writable pages, put our code into them and then make them
// executable, but not writable. That way the pages are never both writable and
// executable at the same time (this is known as W^X and even strictly enforced
// by some operating systems).
//
// Apart from the checks the below function is mostly a copy of the one from
// Peter Cawley's DynASM tutorial, we just do more checking with asserts and try
// to support Macs.
//
// What the function does, is that it receives a dynasm state, with some
// snippets already put into it with `dasm_put` which is essentially a first
// pass over the code, where we are just pasting together snippets and DynASM is
// interpolating them with the evaluations of the used expressions. `dasm_link`
// is the second pass we need to do on the pending code. The call to the
// function essentially tells dynasm that here our assembly pasting ends. It
// will do a pass over the code determining the relative offsets of labels, etc.
// and most importantly it will already figure out the final length of the
// code and return it to use through an output parameter. We use that size to
// allocate enough writable pages from the operating system and tell DynASM to
// encode the code there. This will not only allow us to mark those pages
// executable and have the code in an executable area, but also allows DynASM to
// know the final destination of the code, so it can finalize relative offsets
// to e.g. global labels, which (in general) are not in the pending piece of
// code. After we have the code in our buffer, we ask the operating to change
// the protection of the pages there to make the area non-writable, but
// executable. Finally we return a pointer (`void *`) to the start of the
// buffer. This leaves our function generic and we don't have to limit our
// linking and encoding to a single function signature -- the caller can cast in
// any way they like.
static void *
our_dasm_link_and_encode(Dst_DECL)
{
	size_t size;
	void* code;
	assert(dasm_checkstep(Dst, 0) == DASM_S_OK);
	assert(dasm_link(Dst, &size) == DASM_S_OK);
#ifdef _WIN32
	code = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
	code = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT_VALUE, -1, 0);
#endif
	assert(dasm_encode(Dst, code) == DASM_S_OK);
#ifdef _WIN32
	DWORD original;
	VirtualProtect(code, size, PAGE_EXECUTE_READ, &original);
#else
	mprotect(code, size, PROT_READ | PROT_EXEC);
#endif
#if defined(__APPLE__) && defined(__MACH__)
	sys_icache_invalidate(code, size);
#endif
	return code;
}


// All instructions implicitly bump the instruction pointer by their length
// after they are executed, unless stated otherwise.
enum op {
	// Take 4 bytes from the instruction stream, interpret them as little
	// endian two's complement signed integer and push them on to the stack.
	OP_CONSTANT,

	// Pop `b` and `a` respectively from the stack, push `a + b` on to the
	// stack.
	OP_ADD,

	// Pop a value from top of the stack and print it.
	OP_PRINT,

	// Take a value from the input stream and advance the input position.
	OP_INPUT,

	// Pop a value from top of the stack and don't use it for anything.
	OP_DISCARD,

	// Take 4 bytes from the instruction stream, interpret them as little
	// endian two's complement signed integer to be used as an offset
	// relative to the top of the stack (i.e. offset 0 corresponds to the
	// 32 bit value on top of the stack, offset 1 corresponds to the 32 bit
	// value 1 below the top of the stack). Push the value found on that
	// offset (relative to the top of the stack) on top of the stack.
	OP_GET,

	// Take 4 bytes from the instruction stream, interpret them as little
	// endian two's complement signed integer to be used as an offset
	// relative to the top of the stack (i.e. offset 0 corresponds to the
	// 32 bit value on top of the stack, offset 1 corresponds to the 32 bit
	// value 1 below the top of the stack). Pop a value from top of the
	// stack. Assign the value to the slot at the offset (relative to the
	// top of the stack).
	OP_SET,

	// Pop `b` and `a` respectively from top of the stack. Compare them and
	// push a positive value to the top of the stack if `a` is bigger then
	// `b`, negative value if `b` is bigger than `a` and 0 if `a` is equal
	// to `b`.
	OP_CMP,

	// Take 4 bytes from the instruction stream, interpret them as little
	// endian two's complement signed integer to be used as an offset in the
	// instruction stream. Pop a value from top of the stack. If the value
	// is positive, then apply the offset (in bytes) to the current
	// instruction pointer. Note that the offset is relative to the start
	// of the `OP_JGT` instruction, i.e. the instruction pointer is only
	// bumped by the length of the instruction in case the conditional
	// jump isn't executed.
	OP_JGT,

	// Halt the execution of the program.
	OP_HALT,
};

static void *
compile(u8 *program, size_t program_len)
{
	// Here is the promised local variable holding the `dasm_State *`
	// itself. Though as defined with the macros above, we and other
	// functions will generally use it with the name `ds` and expect a
	// double pointer.
	dasm_State *dasm_state;
	dasm_State **ds = &dasm_state;

	// Each state has to be initialized with a call to `dasm_init`. As
	// promised, we use the macro `Dst`, instead of referring to `ds`
	// directly. This gives us more consistency with rest of the functions
	// and calls and is flexible. In any case the name `Dst` is referred to
	// by the `dasm_put` calls generated by the preprocessor. The second
	// argument tells DynASM the number of sections we will be using. We
	// only need 1, the section for all our code, but we also diligently used
	// the `.section` directive above and told DynASM what names we wanted
	// to give to our sections (we chose "code" for our single one) and thus
	// now have available the macro DASM_MAXSECTION generated by the
	// preprocessor.
	dasm_init(Dst, DASM_MAXSECTION);

	// We are not using global labels and in a simple template JIT likely
	// won't find the need, but we sill need to setup the globals, because
	// local labels can only be used when global labels are setup. And local
	// labels come handy pretty quickly (our snippets may need to contain
	// loops for example).
	void *our_dasm_labels[DASM_LBL__MAX];
	dasm_setupglobal(Dst, our_dasm_labels, DASM_LBL__MAX);

	// Now that we have our dynasm state initialized, we potentially want to
	// reuse it to assemble multiple pastings of templates and not just one.
	// Calling `dasm_init` and `dasm_free` everytime is an option, but we
	// can just reuse the same state. We just have to initialize each
	// "trace" with a call to `dasm_setup`, which we have to do even if we
	// want to process a single "trace" like we are doing in this example.
	// So here it is. We have to provide the byte array prepared by the
	// preprocessor. There can only be one `.actions` directive per file,
	// which means that all `dasm_put` calls in one file are based on that
	// single actions array (called `our_dasm_actions` in this case), so we
	// really don't want to pass anything other here. (One could
	// probably have different files with different `.actions`, but
	// "templates" from different files couldn't be mixed since DASM stores
	// the current actions in its internal state and it can't be changed in
	// any other way than `dasm_setup`, which resets the state.)
	dasm_setup(Dst, our_dasm_actions);

	// We want to translate jumps from the bytecode to jumps in the assembly
	// code and want dynasm to figure out the (relative) offsets as needed
	// by the instructions. For example our `OP_JGT` instruction has an
	// immediate operand which is the (byte) offset to apply in case a jump
	// should be taken. The machine code encodes similarly a relative offset
	// for a (conditional) jump, but it will be a different one than the one
	// in the bytecode. Since there is a potentially arbitrary number of
	// instructions in the bytecode we want to compile, we can't possibly
	// use local labels (1:, ..., 9:), because there are only 9 of them and
	// already want to be able to use them for local control flow (in one or
	// possible more templates pasted together) such as conditional
	// execution or loops. Global labels are also not suitable, because
	// these have symbolic names and we would have a hard time encoding
	// things like "instruction 5 needs to jump 8 bytes back, which is a
	// beginning of another instruction".
	//
	// DynASM has us covered and offers "pc labels", also called "dynamic
	// labels", which are just positive integers (internally used as
	// indices). These labels are dynamic, because the integers are
	// determined at the time of `dasm_put`, i.e. they can be arbitrary C
	// expressions that can evaluate to different values at different times
	// the snippet is pasted (also called "encoding-time constants"). So
	// again same shape (snippet), but with different values. We will be
	// using these labels in a really straightforward way - we'll want to
	// have a dynamic label for each instruction. When we encounter a jump
	// instruction, we can just point the jump to the dynamic label of the
	// destination. This works fine for backward as well as forward jumps,
	// as with all labels, DynASM is able to resolve relative offsets,
	// because it does multiple passes on the code. This is even simpler
	// than our compiler, which had to "fixup" forward jumps!
	//
	// Because the dynamic labels are indices to a DynASM internal array, we
	// need to "preallocate" the ones we'll need, and if we ever need more,
	// we'll have to allocate more. This is done with call to `dasm_growpc`.
	// After call to `dasm_growpc(Dst, N)`, dynamic labels 0 through at
	// least N - 1 will be available to us.
	//
	// Our bytecode is a compact serialization - instructions have different
	// lengths and jumps are based on byte offsets. We will go for a simple
	// route and instead of somehow numbering the instructions from 0 to
	// the number of instructions (and then figuring out the forward jump
	// destinations in a second pass), we'll just have a dynamic label for
	// each _byte_. Then even for forward jumps we are able to calculate
	// their byte offset in the instruction stream, where we will be put an
	// appropriate dynamic label later, and DynASM will resolve it in its
	// second pass. Having a label for each _byte_ is potentially
	// really wasteful as a lot of instructions have lengthy immediates, but
	// it's really simple. (The dynamic labels and the backing array are
	// also reused for multiple runs, if we don't `dasm_free` immediately,
	// but just `dasm_setup` before each run).
	dasm_growpc(Dst, program_len);

	// Now we have a fully initialized DynASM state for this round of
	// pasting together some assembly snippets. Remember that the lines with
	// assembly preceded with `//|` will be translated to calls to
	// `dasm_put(...)`.  So indeed what we are just doing is by controlling
	// the C execution we determine which assembly snippets we want to paste
	// together. With DynASM we can form arbitrary pieces of code, but we
	// somehow need to execute the code. We of course know how to execute
	// some other code in assembly - jump to it or call it. Both essentially
	// do a similar thing - through a relative offset or absolute address
	// stored in a register they change the instruction pointer (and call
	// additionally pushes to the stack the previous instruction pointer
	// which pointed to the _next_ instruction, so after we return from the
	// call, we can just resume the execution).

	// But we want to also somehow jump to our code from C. Unlike the
	// popular belief, the semantics of "goto statements" in C are not such
	// low level as a jumps in assembly, so we'll have to do with function
	// calls. All functions in C respect something that is called the ABI
	// of the platform, the "Application Binary Interface". The set of rules
	// for low level code, so that e.g. functions compiled with one compiler
	// can call functions compiled with another compiler. The ABI is usually
	// specific to both the current processor architecture (e.g. x86-64),
	// because the instruction set is really constraining us in what our
	// "binaries" may look like, as well as the operating system (e.g.
	// Linux), because not only we somehow need to communicate with the
	// operating system, but they historically evolved differently and e.g.
	// have different needs, so the ABI differs. Importantly for us, the ABI
	// defines sizes of C integer types (e.g. `long` is 64-bit on x86-64
	// Linux, but 32-bit on x86-64 Windows) as well as how parameters and
	// return values are passed. For simplicity we will care only about
	// the System V x86-64 ABI, used notably by Linux and other operating
	// systems.
	//
	//     https://gitlab.com/x86-psABIs/x86-64-ABI
	//     https://gitlab.com/x86-psABIs/x86-64-ABI/-/jobs/artifacts/master/raw/x86-64-ABI/abi.pdf?job=build
	//
	// Notably, Windows has a custom x86-64 ABI:
	//
	//     https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170
	//
	// See also a blog post by Eli Bendersky:
	//
	//     https://eli.thegreenplace.net/2011/09/06/stack-frame-layout-on-x86-64

	// Here is the System V ABI 101 (imprecise, but enough to get going):
	//
	// First 6 arguments are passed in registers (left-to-right):
	//
	//     1. rdi
	//     2. rsi
	//     3. rdx
	//     4. rcx
	//     5. r8
	//     6. r9
	//
	// The return value is expected to be in the register rax. If there are
	// more than 6 arguments they pushed on to the stack right-to-left. Even
	// arguments that are smaller than 64 bits use the full registers to be
	// passed. Small-ish structs are passed as if all the struct members
	// were passed as arguments separately.
	//
	// These registers are _callee_ saved (i.!e. the function that called us
	// expects the values in these registers to be preserved, so we, the
	// callee, need to preserve the registers by not using them or restoring
	// them to their original values):
	//
	//     rbp ("base pointer")
	//     rsp ("stack pointer")
	//     rbx
	//     r12
	//     r13
	//     r14
	//     r15
	//
	// These registers are _caller_ saved (i.e. we as the callee can use
	// them freely, since if the caller needs the their values they need to
	// save the values anyway, on the other hand if we call some other
	// function, we are the caller and the function can possibly change any
	// of the registers, so we have to save the values if we need them):
	//
	//     rdi
	//     rsi
	//     rdx
	//     rcx
	//     r8
	//     r9
	//
	//     rax
	//     r10
	//     r11
	//
	// Notably all registers used for parameter passing are caller saved.
	// Obviously the return value register is also caller saved, but note
	// that the callee is free to change it also in the case there is no
	// return value ("the functions returns void").

	// Now is a good time to decide on how and where to store the data we
	// need. We won't need the program at runtime, since it will be
	// hardcoded into the code (including the immediates, etc.), but we will
	// need the stack (since we will just translate the bytecode for stack
	// machine into assembly, not change its workings substantially) and
	// also a pointer to the current input position.
	//
	// The pointer to inputs will be passed to us as an argument - this is
	// the runtime data that our program will process. The `OP_INPUT`
	// instructions take one 32 bit number from the input and advance the
	// input pointer (by 4 bytes). We assume that the caller provided
	// sufficient number of inputs for the program at hand, so we don't
	// necessarily need the length of the input nor check it at runtime.
	// Since it is our only argument it will be in rdi, but that is a caller
	// saved register and when we later execute `OP_PRINT`, we are the
	// caller, so we need to save the register. We could either `push` and
	// `pop` rdi around each call, or move the argument to a callee saved
	// register, which we can save once in the entry to the function and
	// restore once at the end of the function. This makes for a much
	// simpler book keeping, though as always, if we save multiple callee
	// saved registers with `push`, we have to restore them with `pop` in
	// the _reverse_ order.
	//
	// For stack, we'll just use the x86-64 (call) stack. `OP_PUSH` will
	// translate directly to `push` and `OP_POP` directly to `pop`. These
	// instructions change the value of `rsp`, which though a "general
	// purpose register" is usually used as the stack pointer exactly
	// because some instructions can manipulate it compactly (`call` is
	// another example of an instruction that implicitly changes the stack
	// pointer by pushing the return address). So for us `rsp` will
	// represent pointer to the top of the stack, and we will have to
	// embrace its constraints. In particular `rsp` points to the position
	// where the current top of the stack _is_, not one past it like other
	// schemes do, and the stack grows downwards. So top of the stack is at
	// offset zero from top of the stack, and if we want to push a value to
	// the stack, we first need to decrement the stack pointer and then put
	// the value at the stack pointer position, while to pop we need to
	// read the value from the current stack pointer and then increment it.
	// I.e. pre-decrement push and post-increment pop (we would get another
	// combination if the stack pointer pointed one past the top of the
	// stack, or the stack grew upwards). While we have to keep that in
	// mind, this is what the `push` / `pop` instructions will do for us,
	// and we won't have to do it manually). See also another blog post by
	// Eli Bendersky, which has illustrations:
	//
	//     https://eli.thegreenplace.net/2011/02/04/where-the-top-of-the-stack-is-on-x86/
	//
	// (Note that the blog post concerns x86 whose stack operations are 32
	// bit, while on x86-64 stack operations are 64 bit, e.g. push
	// instructions decrement the stack by 8).

	// Here comes the entry to our function, the "prologue". Here we usually
	// want to save all the callee saved registers that we intend to use.
	// Usually prologue starts with "push rbp" and "mov rbp, rsp", which
	// establish the "stack frame" (also called "call frame"), see the
	// linked blog posts by Eli Bendersky for details. Instead we start by
	// pushing rbx. We will use this callee save register to store the input
	// pointer -- while the caller passes us the input pointer as the first
	// argument in rdi, it would get clobbered once we call other function
	// ourselves. Storing it in a callee saved register and
	// storing/restoring the register on entry/exit keeps the management
	// simple. Callee The thing is that the execution of the program may
	// leave some values on the stack. By resetting the rbp we could get
	// easily rid of them, though it would need for us to address
	// relatively to the rsp to restore rbx if it were pushed after rbp.
	// So we simply push it before (and this restore it with pop after we
	// restore the base pointer).

	//| push rbx
	//| push rbp
	//| mov rbp, rsp
	//| mov rbx, rdi

	// Now we will go through all instructions and translate them one by
	// one. For each instruction we have a snippet of assembly, that will be
	// interpolated with value computed from embedded C expressions
	// ("encoding-time constants").
	u8 *instrptr = program;
	u8 *end = program + program_len;

	// This macro is a helper, which reads a 4 byte immediate operand from
	// a position one byte behind the instruction pointer (which skips the
	// opcode).
	#define OPERAND() ((i32) ( \
		((u32)instrptr[1] << 0) | \
		((u32)instrptr[2] << 8) | \
		((u32)instrptr[3] << 16) | \
		((u32)instrptr[4] << 24)))

	while (instrptr < end) {

		// Read the current opcode, which distinguishes the current
		// instruction.
		enum op op = (enum op)*instrptr;

		// Put here a label corresponding to this instructions byte
		// offset. Any preceding or following jumps to this instruction
		// will find this label. Note that we cast the offset to an
		// `int`. This is because DynASM expects all arguments to
		// `dasm_put` to `int`s. As mentioned above, in this ABI `int`
		// is 32 bit, so all values passed to DynASM are limited to the
		// 32 bit range. This is mostly OK, since in the x86-64 most
		// immediates encoded in the instruction stream are also limited
		// to 4 bytes. This 32 bit limitation is thus a limitation of
		// our target architecture, not necessarily of DynASM, and we
		// have to somehow deal with that.
		//
		// Note that in this case, it's unlikely that we would have
		// programs larger than 4 gigabytes, i.e. offset would not fit
		// into 32 bits. But even shorter programs could produce x86-64
		// executable code larger than 4 gigabytes, and there 32 bit
		// relative offsets for jumps and calls would also not be
		// enough.
		//
		// Also at the start of the instruction, there is a (commented
		// out) `int3` instruction. Comment with `//!` (i.e. two forward
		// slashes and exclamation mark) is not a special syntax
		// understood by DynASM, just something different than `//|`,
		// but still a C comment. The `int3` instruction is a "trap to
		// the debugger". So if you were to uncomment it and run this
		// program in debugger, the debugger would stop before each
		// executed (jitted) bytecode instruction. This can be very
		// useful for debugging, also if paired with a print of some
		// debug information before/after each trap.

		int offset = (int) (instrptr - program);
		//|=> offset:
		//! int3

		// The above  will be translated to roughly:
		//
		//         dasm_put(..., offset);
		//
		// We could have also said:
		//
		//         |=> (int) (instrptr - program)
		//
		// Which would translate to:
		//
		//         dasm_put(..., (int) (instrptr - program));
		//
		// There is nothing much more magic about the embedded C
		// expressions - DynASM handles them textually, and they are
		// evaluated at runtime like ordinary C expressions and passed
		// as arguments to `dasm_put`. Note that officially the
		// evaluation order of arguments is undefined in C, so it might
		// a good idea to explicitly evaluate the expressions before
		// using them in assembly snippets, if they can have side
		// effects, and even if they don't.

		switch (op) {
		case OP_CONSTANT: {
			// If the next instruction is `OP_CONSTANT` we need to
			// append the code for it, we do it by having an
			// assembly snippet here, which translates to
			// `dasm_put`. The instruction has an immediate operand
			// which we read with a macro and store it in a variable
			// before passing it to the snippet. While the
			// `OP_CONSTANT` instruction is generic and an
			// interpreter has to load the constant from bytecode
			// everytime, this particular `OP_CONSTANT` will always
			// have this one same operand, so we can just
			// embed it in the code. The below translates to `push 4`
			// if the operand in the instruction stream is
			// the number 4. This will be pattern matched by DynASM
			// to be a push of 32 bit immediate (see the unofficial
			// DynASM instruction reference or other x86-64
			// instruction references). This is perfect for us,
			// since we want to push a 32 bit value. If we wanted
			// to push 64 bit value, we would first have to somehow
			// get the value to a register (see later) and then use
			// `push REG`, because there just isn't an instruction
			// that is able to push 64 bit immediates. DynASM won't
			// do that for us automatically. What's worse the
			// argument (passed as vararg) is expected to be an int,
			// so passing larger values will only result in silent
			// truncation. Beware!

			i32 operand = OPERAND();
			//| push operand

			// This instruction is 5 bytes long, we have to advance
			// the instruction pointer by 5 bytes and switch on the
			// opcode to decide on what to compile next.

			instrptr += 5; break;
		}
		case OP_ADD: {
			// Pop the second argument, pop the first argument, add
			// the second to the first and push their result. The
			// operations here are 64 bit. Since stacks work with 64
			// bit values, the `pop`s and `push`es have to be 64 bit,
			// but the arithmetic doesn't -- we could just as well
			// have written `add eax, ecx` to operate on 32 bit
			// parts of the registers (which will zero out the
			// upper 32 bits). It would have save us a byte in the
			// encoding of the instructions and be slightly faster
			// in runtime (adding 32 bits is faster than adding 64),
			// but we don't care about such small improvements.

			//| pop rcx
			//| pop rax
			//| add rax, rcx
			//| push rax

			instrptr += 1; break;
		}
		case OP_PRINT: {
			// Printing is a little tricky. Barring system calls, we
			// have to call an external function to do the print.
			// This could be either our own function, or
			// even one from external library, like `printf`.
			// There is one problem though normal calls are based on
			// signed 32 bit relative offsets, while the x86-64
			// space is 64 bit, so call targets can be much further
			// than +-2 GiB. Here we find ourselves at runtime,
			// where both the static and dynamic loader/linker
			// have already figured out where everything in the
			// memory is, after all, our program is already
			// running. So for example the (64 bit) address of the
			// `printf` function is already known. But since we have
			// not yet mmaped pages for our code, we don't know
			// whether it will be close to `printf`!. DynASM has the
			// capability to handle external names through a
			// callback which will let us handle that in the
			// encoding stage where we already know the
			// destination of our code, but let's not go that far
			// right now. What we can do is use an _indirect_
			// through a register - we will store the 64 bit address
			// of `printf` in a register, and then call it
			// with `call REG`. Most instructions don't allow 64 bit
			// immediate operands, but one does, DynASM calls it
			// fittingly `mov64`, but other assemblers call it
			// `movabs`. It can store a 64 bit immediate in a
			// register. Perfect for us. Though there is another
			// problem, since the parameters to `dasm_put` are all
			// 32 bit integers (`int`), how are we going to pass it
			// to DynASM? Well, DynASM preprocessor knows that the
			// instruction operand is special and handles it by
			// passing the lower 32 bits and upper 32 bits
			// separately. Since the argument to `dasm_put` is
			// determined by a simple text substitution, and here
			// the argument will be evaluated twice, we should be
			// even more careful about complex expressions and side
			// effects.
			//
			// To printf, we also have to pass the format string as
			// a second argument, we also take its address and move
			// it into a register as 64 bit immediate. The second
			// argument should be the number to print -- we pop that
			// from the stack into a register. Finally we call the
			// function with an indirect call instruction.
			// Note that we cast the addresses to integers to avoid
			// compiler warnings (because dynasm follows it with
			// truncation to 32 bits or extraction of upper 32 bits
			// respectively.
			//
			// It is important to note, that printf respects the
			// ABI, so we have to presume it destroys the
			// values in caller saved registers, we only store our
			// state in rbx, so we our fine.

			//| mov64 rdi, ((uintptr_t) "%zd\n")
			//| pop rsi
			//| mov64 rax, ((uintptr_t) printf)
			//| call rax

			// The above is translated to roughly the following:
			//
			//     dasm_put(...,
			//         (unsigned int)(((uintptr_t) "%zd\n")),
			//         (unsigned int)((((uintptr_t) "%zd\n"))>>32)
			//         (unsigned int)(((uintptr_t) printf)),
			//         (unsigned int)((((uintptr_t) printf))>>32),
			//     )

			instrptr += 1; break;
		}
		case OP_INPUT: {
			// We have to read 32 bits from the place where rbx (the
			// input pointer) points to and advance it by
			// those 32 bits so next time we read the next input
			// value.
			//
			// For reading a 32 bit value (aka dword) from memory,
			// we could just issue a read into some 32 bit
			// register, like `eax`, i.e. `mov eax, [rbx]`, which
			// says "read a 32 bit value from a 64 bit address
			// stored in the rbx register. The size of the read is
			// determined implicitly from the (in this case
			// destination) register size. In cases involving
			// immediates, we usually have to specify whether we are
			// storing an 8/16/32/64 bit immediate by including the
			// right keyword for the size (`byte`, `word`, `dword`,
			// `qword` respectively). But we can also include these
			// keywords when they are not necessary, if we prefer
			// that, as we do below by specifying "dword read", even
			// though it would have been implicit from the use of
			// `eax` as the destination.

			//| mov eax, dword [rbx]
			//| push rax
			//| add rbx, 4

			instrptr += 1; break;
		}
		case OP_DISCARD: {
			// We just `pop` into a register and don't use the
			// value. That is what `OP_DISCARD` really is. Okay,
			// since we don't do anything interesting with the
			// value, we could just increment the stack pointer with
			// `add rsp, 8`, which would have saved a load to `rax`,
			// but since we don't care about preserving `rax`, and
			// use `push` and `pop` instructions in other places, so
			// we do it for consistency.

			//| pop rax

			instrptr += 1; break;
		}
		case OP_GET: {
			// For `OP_GET` we have a 32 bit immediate operand which
			// we will have to use as an offset to the top of the
			// stack. The offset is in number of elements, and in
			// our case each elements is 8 bytes. Since we load the
			// both the constant 8 and the operand as well as the
			// result of their multiplication are runtime known
			// values, but constants from the point of view of the
			// instruction - the multiplication is performed now and
			// the instruction will only contain the offset
			// as an immediate, for example if operand is 5, the
			// instruction will have the immediate value 40 encoded
			// in it. The way we do that below is slightly risky, we
			// put the multiplication and read of the operand in the
			// assembly snippet. The syntax may seem similar to the
			// well known complex addressing modes of x86-64, where
			// for example `[rsp + 8 * rax]` would mean of the
			// possible addressings. The trick here is that DynASM
			// expects to see `[rsp + rax * 8]`, so it isn't
			// confused by the leading `8 *`. Since `8 * OPERAND()`
			// is an encoding time constant, an entirely different
			// addressing mode will be used `[REG + IMMEDIATE]`.
			// Evaluating the "constant" before the assembly snippet
			// and storing it in a variable would have probably made
			// it much clearer.

			//| mov rax, [rsp + 8 * OPERAND()]
			//| push rax

			// The above translates roughly to:
			//
			//     dasm_put(..., 8 * OPERAND());

			instrptr += 5; break;
		}
		case OP_SET: {
			// There isn't anything interesting to `OP_SET`, we
			// pop a value from top of the stack and then store it
			// to an offset from (the new) top of the stack.

			//| pop rax
			//| mov [rsp + 8 * OPERAND()], rax

			instrptr += 5; break;
		}
		case OP_CMP: {
			// Here we need to compare two numbers and push a
			// positive number / negative number / zero as
			// appropriate. Let's not use setcc, and instead
			// showcase some local control flow using local labels.
			// Remember, the "arrows" (`<`, `>`) of local labels
			// point in the direction where the label is defined,
			// here we refer to forward labels only. The labels are
			// also purely based on the order of the instructions,
			// i.e. the order dasm receives them in, lexical order
			// in the C source doesn't matter at all.

			//| pop rcx
			//| pop rax
			//| cmp rax, rcx
			//| jg >1
			//| je >2
			//| push -1
			//| jmp >3
			//|1:
			//| push 1
			//| jmp >3
			//|2:
			//| push 0
			//|3:
			instrptr += 1; break;
		}
		case OP_JGT:
			// Conditional branch based on whether the top of the
			// stack is a positive number. We set the flags based on
			// the top of the stack by popping it into a register
			// and then testing the register with itself. This
			// effectively compares the register to zero, so we can
			// use the "greater than" conditional jump. The relative
			// byte offset to the destination is encoded in
			// the instruction stream, so we figure out the
			// (absolute) byte offset from the beginning, which also
			// is the index of dynamic label we either already put
			// there, or will later.

			int offset = (int) (instrptr - program + OPERAND());
			//| pop rax
			//| test rax, rax
			//| jg => offset

			instrptr += 5; break;
		case OP_HALT:
			// When the code compiled by us encounters this
			// instruction, it should halt the execution, since we
			// structure the code as a function, here is the right
			// place for return -- an epilogue including a `ret`
			// instruction.
			//
			// In our scheme we decided to first save rbx and then
			// rbp/rsp, so here we do the reverse - restore the base
			// and stack pointers and then restore rbx to the
			// caller's value.

			//| mov rsp, rbp
			//| pop rbp
			//| pop rbx
			//| ret

			instrptr += 1; break;
		}
	}

	// We `dasm_put` all snippets. Now we need to link and encode them. See
	// the description of the function for more details.
	void *code = our_dasm_link_and_encode(Dst);

	// Here we could keep the same DASM state, call `dasm_setup` and
	// continue with compilation of other programs, we just needed this
	// single one. (Also in the case we wanted to compile more programs, we
	// would probably split the dasm state initialization and
	// deinitialization into separate functions called just once, which
	// would have made it possible to use the same state with the `compile`
	// function multiple times.
	dasm_free(Dst);
	return code;
}

int
main(int argc, char **argv)
{
	// Here we have a program to multiply two numbers, the entire program,
	// just like the bytecode is due to Martin Dørum, be sure to check his
	// blog post for details:
	//
	//         https://mort.coffee/home/fast-interpreters/

	u8 program[] = {
		OP_INPUT, OP_INPUT,
		OP_CONSTANT, 0, 0, 0, 0,

		OP_GET, 0, 0, 0, 0,
		OP_GET, 3, 0, 0, 0,
		OP_ADD,
		OP_SET, 0, 0, 0, 0,

		OP_GET, 1, 0, 0, 0,
		OP_CONSTANT, 0xff, 0xff, 0xff, 0xff, // -1 32-bit little-endian (two's complement)
		OP_ADD,
		OP_SET, 1, 0, 0, 0,

		OP_GET, 1, 0, 0, 0,
		OP_CONSTANT, 0, 0, 0, 0,
		OP_CMP,
		OP_JGT, 0xd5, 0xff, 0xff, 0xff, // -43 in 32-bit little-endian (two's complement)

		OP_GET, 0, 0, 0, 0,
		OP_PRINT,

		OP_HALT,
	};

	// The input for us are just two command line arguments.
	if (argc != 3) {
		fprintf(stderr, "Expected exactly 2 arguments\n");
		return 1;
	}
	i32 input[] = { atoi(argv[1]), atoi(argv[2]) };

	// Compile the program by calling the compile function with the program
	// and its size.
	void (*fun)(i32 *input) = compile(program, sizeof(program));

	// And run the function passing it pointer to the input array. We don't
	// pass the length, the compiled program doesn't need it -- it expects
	// the input array to be of sufficient length.
	fun(input);

	return 0;
}
