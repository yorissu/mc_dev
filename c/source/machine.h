
#ifndef __mc__machine_h__
#define __mc__machine_h__

/// the virtual machine's internals, shared by its source files:
///
///   vm.c           loading a module, the instruction loop, runtime errors
///   vm_memory.c    blocks and handles, frames, constants, values in memory
///   vm_plugins.c   calls of plugin functions (intrinsics): the run-time plugin api
///
/// the design (see also bytecode.h):
///
/// - the operand stack is an array of 64-bit words. a scalar takes one
///   word (integers sign- or zero-extended from their type, f32 as a double,
///   bools 0 or 1, function values their index with the top bit set for
///   intrinsics); a slice or a location two (a handle and an offset, plus
///   the length for slices); a struct or an array its c bytes, rounded up to
///   whole words. aggregates are moved with memcpy.
/// - memory is in blocks: the byte area of a call (on a stack of chunks
///   that never move, so addresses into it stay valid while the call runs),
///   heap allocations (calloc), and the images of constants. a block is an
///   entry of the table of its kind; its handle is a 64-bit word:
///
///       bits 0-31 index, 32-60 generation, 61 read-only, 62-63 kind
///
///   an entry's generation changes when its block dies (free, realloc, the
///   call returns) and the entry is reused later, so an old handle no longer
///   matches: that is the use-after-free check, and the kind of the handle
///   says which message to give. a slice's second word is its byte offset
///   (low 32 bits) and its length (high 32 bits); a location's second word
///   is its byte offset.
/// - a frame gets a block only when a location or a slice of its locals is
///   taken (LOC_LOCAL, TEMP_LOC), so most calls never touch the tables.
/// - there is no garbage collector: nothing on the operand stack or in a
///   frame owns memory except heap blocks, which 'free' gives back; the
///   rest is freed when the machine stops.

#include "mc/vm.h"
#include "plugin_host.h"

#include <setjmp.h>
#include <stdarg.h>
#include <string.h>

typedef union {
	int64_t  i;
	uint64_t u;
	double   f;
} mc_word_u;

typedef enum {
	mc_block_none  ,   // the null slice
	mc_block_heap  ,   // from alloc, until free or realloc
	mc_block_frame ,   // the locals of a call, until it returns
	mc_block_static,   // constants, for the whole run
	mc_block_kind_count_,
} mc_block_kind_e;

#define mc_handle_read_only ((uint64_t)1 << 61)
#define mc_handle_generation_mask 0x1FFFFFFFu

static inline mc_block_kind_e mc_handle_kind(const uint64_t handle) {
	return (mc_block_kind_e)(handle >> 62);
}

static inline uint32_t mc_handle_index(const uint64_t handle) {
	return (uint32_t)handle;
}

static inline uint32_t mc_handle_generation(const uint64_t handle) {
	return (uint32_t)(handle >> 32) & mc_handle_generation_mask;
}

static inline uint64_t mc_handle_make(const mc_block_kind_e kind, const uint32_t index, const uint32_t generation) {
	return ((uint64_t)kind << 62) | ((uint64_t)(generation & mc_handle_generation_mask) << 32) | index;
}

/// the second word of a slice.
static inline uint64_t mc_slice_word(const uint64_t offset, const uint64_t length) {
	return (offset & 0xFFFFFFFFu) | (length << 32);
}

static inline uint64_t mc_slice_offset(const uint64_t word) {
	return word & 0xFFFFFFFFu;
}

static inline uint64_t mc_slice_length(const uint64_t word) {
	return word >> 32;
}

typedef struct {
	uint8_t* data      ;
	uint64_t size      ;
	uint32_t generation;
	uint32_t next_free ;   // the next free entry (0: none), while free
	bool     alive     ;
	void   (*release)(void* const memory);   // heap blocks: how to give the memory back
} mc_block_s;

typedef struct {
	mc_block_s* items    ;   // entry 0 is never used: index 0 is no block
	uint32_t    count    ;
	uint32_t    capacity ;
	uint32_t    free     ;   // the first free entry (0: none)
} mc_blocks_s;

/// a chunk of the stack that frames take their byte areas from.
typedef struct {
	uint8_t* data;
	size_t   size;
	size_t   used;
} mc_chunk_s;

typedef struct mc_exec_s mc_exec_s;

/// a function, checked and decoded for running.
typedef struct {
	mc_exec_s* code     ;
	size_t     max_stack;   // words of operand stack it needs at most
	uint64_t   arguments;   // words of its arguments
	bool       packed   ;   // the arguments are words at offsets 0, 8, 16...: one memcpy moves them
} mc_prepared_s;

typedef struct {
	int64_t                 function  ;
	const mc_exec_s*        code      ;
	int64_t                 pc        ;
	uint8_t*                base      ;   // the byte area of the frame
	uint64_t                handle    ;   // its block, 0 until one is needed
	size_t                  chunk     ;   // where the byte area was taken from
	size_t                  used      ;   // the chunk's use before it
	size_t                  stack_base;   // the operand stack below the call
} mc_frame_s;

typedef struct mc_vm_s mc_vm_s;

/// an intrinsic linked to the plugin function that provides it.
typedef struct {
	const mc_plugin_function_s* function    ;
	const mc_bc_intrinsic_s*    intrinsic   ;
	size_t                      count       ;   // arguments, the values for '...' included
	size_t                      fixed       ;
	const mc_plugin_type_s**    types       ;   // of every argument
	size_t*                     offsets     ;   // the first word of every argument
	const mc_plugin_type_s*     result      ;   // null: none
	uint64_t                    result_words;
} mc_vm_link_s;

/// a constant, ready to use: its words on the operand stack and its block
/// (for locations of it; made on first use).
typedef struct {
	mc_word_u* words ;
	size_t     count ;
	uint64_t   handle;
} mc_vm_constant_s;

struct mc_vm_s {
	const mc_module_bc_s*        module     ;
	const mc_vm_options_s*       options    ;
	FILE*                        out        ;
	// the operand stack
	mc_word_u*                   stack      ;
	size_t                       sp         ;
	size_t                       capacity   ;
	// calls
	mc_vector_of(mc_frame_s)     frames     ;
	mc_vector_of(mc_chunk_s)     chunks     ;
	size_t                       chunk      ;   // the chunk in use
	// memory
	mc_blocks_s                  blocks[mc_block_kind_count_];
	mc_vector_of(mc_vm_constant_s) constants;
	mc_vector_of(mc_vm_link_s)   links      ;   // per intrinsic
	mc_plugin_type_s*            types      ;   // a handle per type descriptor
	mc_vector_of(uint8_t)        scratch    ;   // plugin arguments in memory
	mc_vector_of(mc_prepared_s)  prepared   ;   // per function
	mc_vector_of(uint8_t*)       zeros      ;   // the zero value of every type, made on first use (null: all zero bytes)
	mc_string_s                  buffer     ;   // what an intrinsic prints
	mc_string_s                  bytes      ;
	uint64_t                     steps      ;   // instructions executed
	uint64_t                     allocations;
	bool                         returned   ;
	mc_word_u                    result     ;
	// stopping
	jmp_buf*                     stop       ;
	char*                        error      ;
};

#define mc_call_depth_limit 10000

// vm.c
/// stops the program with a runtime error.
mc_noreturn void mc_vm_fail(mc_vm_s* const vm, const char* const format, ...) mc_printf_like(2, 3);
mc_noreturn void mc_vm_vfail(mc_vm_s* const vm, const char* const format, va_list args) mc_printf_like(2, 0);

// vm_memory.c
/// a new block of 'kind' over 'data' (the entry is reused when one is free).
uint64_t mc_vm_block_new(mc_vm_s* const vm, const mc_block_kind_e kind, uint8_t* const data, const uint64_t size,
						 void (*release)(void* const memory));

/// ends a block: its handles stop working; heap memory is given back.
void mc_vm_block_end(mc_vm_s* const vm, const uint64_t handle);

/// the memory of a live block; stops with the right message when it died.
uint8_t* mc_vm_resolve(mc_vm_s* const vm, const uint64_t handle);

/// stops because 'handle' cannot be used: the block died, or the access
/// ('size' bytes at 'offset') is outside of it.
mc_noreturn void mc_vm_bad_access(mc_vm_s* const vm, const uint64_t handle);

/// 'size' bytes at 'offset' of a live block (the fast path of every access).
static inline uint8_t* mc_vm_memory(mc_vm_s* const vm, const uint64_t handle, const uint64_t offset, const uint64_t size) {
	const mc_blocks_s* const table = &vm->blocks[handle >> 62];
	const uint32_t index = (uint32_t)handle;
	if (index < table->count) {
		const mc_block_s* const block = &table->items[index];
		if (block->generation == ((uint32_t)(handle >> 32) & mc_handle_generation_mask) && offset <= block->size && size <= block->size - offset) {
			return block->data + offset;
		}
	}
	mc_vm_bad_access(vm, handle);
}

/// the entry of a handle, or null when the block died (or never existed).
mc_block_s* mc_vm_block_of(mc_vm_s* const vm, const uint64_t handle);

/// the operand stack has room for 'count' more words.
void mc_vm_reserve(mc_vm_s* const vm, const size_t count);

/// the byte area of a new frame / of the frame being left.
uint8_t* mc_vm_frame_alloc(mc_vm_s* const vm, mc_frame_s* const frame, const uint64_t size);
void mc_vm_frame_release(mc_vm_s* const vm, mc_frame_s* const frame);

/// the handle of a frame's block (made when first needed).
uint64_t mc_vm_frame_handle(mc_vm_s* const vm, mc_frame_s* const frame);

/// words on the operand stack of a value of the type descriptor 'type'.
uint64_t mc_vm_type_words(const mc_vm_s* const vm, const int64_t type);

/// bytes in memory of a value of a type code.
uint64_t mc_vm_code_size(const mc_type_code_e code);

/// a scalar (or a function value) in memory, as a word, and back.
static inline mc_word_u mc_vm_read(const mc_type_code_e code, const uint8_t* const memory) {
	mc_word_u word = { .u = 0, };
	switch (code) {
		case mc_code_s8:   word.i = (int8_t)memory[0]; break;
		case mc_code_u8:   word.u = memory[0]; break;
		case mc_code_bool: word.u = memory[0] != 0; break;
		case mc_code_s16:  { int16_t value; memcpy(&value, memory, 2); word.i = value; break; }
		case mc_code_u16:  { uint16_t value; memcpy(&value, memory, 2); word.u = value; break; }
		case mc_code_s32:  { int32_t value; memcpy(&value, memory, 4); word.i = value; break; }
		case mc_code_u32:  { uint32_t value; memcpy(&value, memory, 4); word.u = value; break; }
		case mc_code_f32:  { float value; memcpy(&value, memory, 4); word.f = (double)value; break; }
		default:           memcpy(&word, memory, 8); break;
	}
	return word;
}

static inline void mc_vm_write(const mc_type_code_e code, uint8_t* const memory, const mc_word_u word) {
	switch (code) {
		case mc_code_s8: case mc_code_u8:   memory[0] = (uint8_t)word.u; break;
		case mc_code_bool:                  memory[0] = word.u != 0; break;
		case mc_code_s16: case mc_code_u16: { const uint16_t value = (uint16_t)word.u; memcpy(memory, &value, 2); break; }
		case mc_code_s32: case mc_code_u32: { const uint32_t value = (uint32_t)word.u; memcpy(memory, &value, 4); break; }
		case mc_code_f32:                   { const float value = (float)word.f; memcpy(memory, &value, 4); break; }
		default:                            memcpy(memory, &word, 8); break;
	}
}

/// the value of an integer word of 'code' as a 128-bit integer (for messages and checks).
__int128 mc_vm_integer(const mc_type_code_e code, const mc_word_u word);

/// 'value' wrapped to the integer type 'code', as a word.
mc_word_u mc_vm_wrap(const mc_type_code_e code, const uint64_t value);

/// whether 'code' is a signed integer type.
bool mc_vm_signed(const mc_type_code_e code);

/// 'value' rounded as the float type 'code' (f32 rounds).
double mc_vm_round(const mc_type_code_e code, const double value);

/// 'value' in decimal (for messages).
const char* mc_vm_int_text(char buffer[48], const __int128 value);

/// the constants of the module, made ready (fails on a broken module).
void mc_vm_load_constants(mc_vm_s* const vm);

/// the handle of a constant's block (read-only).
uint64_t mc_vm_constant_handle(mc_vm_s* const vm, const int64_t index);

/// the zero value of a type in memory (null: all zero bytes).
const uint8_t* mc_vm_zero(mc_vm_s* const vm, const int64_t type);

void mc_vm_free_memory(mc_vm_s* const vm);

// vm_plugins.c
/// links every intrinsic of the module to its plugin function (stops when one is missing).
void mc_vm_link(mc_vm_s* const vm, const mc_plugins_s* const plugins);
void mc_vm_unlink(mc_vm_s* const vm);

/// calls intrinsic 'index': its arguments are the top 'words' words of the
/// stack, and its result replaces them.
void mc_vm_call_plugin(mc_vm_s* const vm, const int64_t index, const int64_t words);

/// words of the arguments and of the result of an intrinsic.
uint64_t mc_vm_intrinsic_arguments(const mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic);
uint64_t mc_vm_intrinsic_result(const mc_vm_s* const vm, const mc_bc_intrinsic_s* const intrinsic);

#endif
