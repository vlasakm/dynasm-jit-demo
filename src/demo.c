#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

typedef unsigned char u8;
typedef int i32;
typedef unsigned int u32;

#if _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#if defined(__APPLE__) && defined(__MACH__)
#define MAP_JIT_VALUE MAP_JIT
void sys_icache_invalidate(void *start, size_t len);
#else
#define MAP_JIT_VALUE 0
#endif

typedef struct dasm_State dasm_State;

typedef struct {
	i32 *stack;
	i32 *input;
	dasm_State *ds;
	void **global_labels;
} InterpreterState;

#define Dst       is
#define Dst_DECL  InterpreterState *is
#define Dst_REF   (is->ds)

#define DASM_CHECKS

#include "dasm_proto.h"

#include "dasm_x86.h"

//|.arch x64
//|.actionlist our_dasm_actions
//|.globals DASM_LBL_
//|.section code

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

enum op {
	OP_CONSTANT,
	OP_ADD,
	OP_PRINT,
	OP_INPUT,
	OP_DISCARD,
	OP_GET,
	OP_SET,
	OP_CMP,
	OP_JGT,
	OP_HALT,
};

void
state_init(InterpreterState *is, i32 *input)
{

	*is = (InterpreterState) {
		.stack = malloc(64 * sizeof(i32)),
		.input = input,
	};

	dasm_init(Dst, DASM_MAXSECTION);
	is->global_labels = malloc(DASM_LBL__MAX * sizeof(is->global_labels[0]));
	dasm_setupglobal(Dst, is->global_labels, DASM_LBL__MAX);
}

void
state_free(InterpreterState *is)
{
	dasm_free(Dst);
	free(is->stack);
	free(is->global_labels);
}

static void *
compile(Dst_DECL, u8 *program, size_t program_len)
{
	dasm_setup(Dst, our_dasm_actions);
	dasm_growpc(Dst, program_len);

	//| .type IS, InterpreterState
	//| .type STACK, int, r12
	//| .type INPUT, int, rbx

	//| push rbp
	//| mov rbp, rsp
	//| push INPUT
	//| push STACK
	//|
	//| mov INPUT, IS:rdi->input
	//| mov STACK, IS:rdi->stack

	u8 *instrptr = program;
	u8 *end = program + program_len;

	#define OPERAND() ((i32) ( \
		((u32)instrptr[1] << 0) | \
		((u32)instrptr[2] << 8) | \
		((u32)instrptr[3] << 16) | \
		((u32)instrptr[4] << 24)))

	while (instrptr < end) {
		enum op op = (enum op)*instrptr;

		int offset = (int) (instrptr - program);
		//|=> offset:
		//! int3

		switch (op) {
		case OP_CONSTANT: {
			int32_t operand = OPERAND();
			//| mov dword STACK[0], operand
			//| add STACK, #STACK
			instrptr += 5; break;
		}
		case OP_ADD: {
			//| mov ecx, STACK[-1]
			//| add STACK[-2], ecx
			//| sub STACK, #STACK
			instrptr += 1; break;
		}
		case OP_PRINT: {
			//| mov64 rdi, ((uintptr_t) "%zd\n")
			//| mov esi, STACK[-1]
			//| sub STACK, 4
			//| mov64 rax, ((uintptr_t) printf)
			//| call rax
			instrptr += 1; break;
		}
		case OP_INPUT: {
			//| mov eax, INPUT[0]
			//| mov dword STACK[0], eax
			//| add STACK, #STACK
			//| add INPUT, #INPUT
			instrptr += 1; break;
		}
		case OP_DISCARD: {
			//| sub STACK, #STACK
			instrptr += 1; break;
		}
		case OP_GET: {
			//| mov eax, STACK[-1 - OPERAND()]
			//| mov STACK[0], eax
			//| add STACK, #STACK
			instrptr += 5; break;
		}
		case OP_SET: {
			//| mov eax, STACK[-1]
			//| sub STACK, #STACK
			//| mov STACK[-1 - OPERAND()], eax
			instrptr += 5; break;
		}
		case OP_CMP: {
			//| mov ecx, STACK[-1]
			//| cmp STACK[-2], ecx
			//| jg >1
			//| je >2
			//| mov ecx, -1
			//| jmp >3
			//|1:
			//| mov ecx, 1
			//| jmp >3
			//|2:
			//| mov ecx, 0
			//|3:
			//| mov STACK[-2], ecx
			//| sub STACK, #STACK
			instrptr += 1; break;
		}
		case OP_JGT: {
			int offset = (int) (instrptr - program + OPERAND());
			//| mov eax, STACK[-1]
			//| sub STACK, #STACK
			//| test rax, rax
			//| jg => offset
			instrptr += 5; break;
		}
		case OP_HALT: {
			//| add rsp, 64 * #STACK
			//| pop STACK
			//| pop INPUT
			//| mov rsp, rbp
			//| pop rbp
			//| ret
			instrptr += 1; break;
		}
		}
	}

	void *code = our_dasm_link_and_encode(Dst);
	return code;
}

int
main(int argc, char **argv)
{
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

	if (argc != 3) {
		fprintf(stderr, "Expected exactly 2 arguments\n");
		return 1;
	}
	i32 input[] = { atoi(argv[1]), atoi(argv[2]) };

	InterpreterState is;
	state_init(&is, input);

	void (*fun)(InterpreterState *is) = compile(&is, program, sizeof(program));

	fun(&is);

	state_free(&is);

	return 0;
}
