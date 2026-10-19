
/// the virtual machine: loading a module (checking and decoding every
/// function), memory, calls of plugin functions, the instruction loop and
/// runtime errors.
///
/// loading verifies each function before it can run: every jump lands on
/// an instruction, the operand stack has the same depth on every path into
/// an instruction and never underflows, every offset into the frame and
/// into values on the stack is inside them, and calls and returns move the
/// words their functions take and give. that is what lets the loop skip
/// those checks, and what keeps a damaged '.miab' file from corrupting
/// memory (everything else, like an access through a slice, is checked
/// when it runs). the maximal depth of the stack found there is reserved on
/// every call, so pushing never has to check for room.
///
/// the loop runs decoded instructions: the frequent ones (word loads and
/// stores, arithmetic on 64-bit integers and doubles, comparisons, jumps,
/// slice elements) get their own cases with the operand stack in a local
/// pointer; the rest go through the general _mia_vm_step. a comparison and
/// its jump, and a few frequent runs of instructions (_mia_vm_fuse), run as
/// one case each, still counted instruction by instruction.
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

#include "mia/format.h"
#include "mia/int128.h"
#include "mia/vm.h"
#include "mia/plugins.h"

#include <inttypes.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

typedef union {
	int64_t  i;
	uint64_t u;
	double   f;
} _mia_vm_word_u;

typedef enum {
	_mia_vm_block_none  ,   // the empty slice (no memory)
	_mia_vm_block_heap  ,   // from alloc, until free or realloc
	_mia_vm_block_frame ,   // the locals of a call, until it returns
	_mia_vm_block_static,   // constants, for the whole run
	_mia_vm_block_kind_count,
} _mia_vm_block_kind_e;

#define _mia_vm_handle_read_only ((uint64_t)1 << 61)
#define _mia_vm_handle_generation_mask 0x1FFFFFFFu

static inline _mia_vm_block_kind_e _mia_vm_handle_kind(const uint64_t handle) {
	return (_mia_vm_block_kind_e)(handle >> 62);
}

static inline uint32_t _mia_vm_handle_index(const uint64_t handle) {
	return (uint32_t)handle;
}

static inline uint32_t _mia_vm_handle_generation(const uint64_t handle) {
	return (uint32_t)(handle >> 32) & _mia_vm_handle_generation_mask;
}

static inline uint64_t _mia_vm_handle_make(const _mia_vm_block_kind_e kind, const uint32_t index, const uint32_t generation) {
	return ((uint64_t)kind << 62) | ((uint64_t)(generation & _mia_vm_handle_generation_mask) << 32) | index;
}

/// the second word of a slice.
static inline uint64_t _mia_vm_slice_word(const uint64_t offset, const uint64_t length) {
	return (offset & 0xFFFFFFFFu) | (length << 32);
}

static inline uint64_t _mia_vm_slice_offset(const uint64_t word) {
	return word & 0xFFFFFFFFu;
}

static inline uint64_t _mia_vm_slice_length(const uint64_t word) {
	return word >> 32;
}

typedef struct {
	uint8_t* data      ;
	uint64_t size      ;
	uint32_t generation;
	uint32_t next_free ;   // the next free entry (0: none), while free
	bool     alive     ;
	void   (*release)(void* const memory);   // heap blocks: how to give the memory back
} _mia_vm_block_s;

typedef struct {
	_mia_vm_block_s* items   ;   // entry 0 is never used: index 0 is no block
	uint32_t         count   ;
	uint32_t         capacity;
	uint32_t         free    ;   // the first free entry (0: none)
} _mia_vm_blocks_s;

/// a chunk of the stack that frames take their byte areas from.
typedef struct {
	uint8_t* data;
	size_t   size;
	size_t   used;
} _mia_vm_chunk_s;

typedef struct _mia_vm_exec_s _mia_vm_exec_s;

/// a function, checked and decoded for running.
typedef struct {
	_mia_vm_exec_s* code     ;
	size_t          max_stack;   // words of operand stack it needs at most
	size_t          frame    ;   // its byte area, rounded up to 16 bytes
	uint64_t        arguments;   // words of its arguments
	bool            packed   ;   // the arguments are words at offsets 0, 8, 16...: one memcpy moves them
} _mia_vm_prepared_s;

typedef struct {
	int64_t               function  ;
	const _mia_vm_exec_s* code      ;
	int64_t               pc        ;
	uint8_t*              base      ;   // the byte area of the frame
	uint64_t              handle    ;   // its block, 0 until one is needed
	size_t                chunk     ;   // where the byte area was taken from
	size_t                used      ;   // the chunk's use before it
	size_t                stack_base;   // the operand stack below the call
} _mia_vm_frame_s;

typedef struct _mia_vm_s _mia_vm_s;

/// an intrinsic linked to the plugin function that provides it.
typedef struct {
	const mia_plugins_function_s*   function    ;
	const mia_bytecode_intrinsic_s* intrinsic   ;
	size_t                          count       ;   // arguments, the values for '...' included
	size_t                          fixed       ;
	const mia_plugin_type_s**       types       ;   // of every argument
	size_t*                         offsets     ;   // the first word of every argument
	const mia_plugin_type_s*        result      ;   // null: none
	uint64_t                        result_words;
} _mia_vm_link_s;

/// a constant, ready to use: its words on the operand stack and its block
/// (for locations of it; made on first use).
typedef struct {
	_mia_vm_word_u* words ;
	size_t          count ;
	uint64_t        handle;
} _mia_vm_constant_s;

struct _mia_vm_s {
	const mia_bytecode_module_s* module ;
	const mia_vm_options_s*      options;
	FILE*                        out    ;
	// the operand stack
	_mia_vm_word_u* stack   ;
	size_t          sp      ;
	size_t          capacity;
	// calls
	mia_vector_of(_mia_vm_frame_s) frames;
	mia_vector_of(_mia_vm_chunk_s) chunks;
	size_t                         chunk ;   // the chunk in use
	// memory
	_mia_vm_blocks_s                  blocks[_mia_vm_block_kind_count];
	mia_vector_of(_mia_vm_constant_s) constants                       ;
	mia_vector_of(_mia_vm_link_s)     links                           ;   // per intrinsic
	mia_plugin_type_s*                types                           ;   // a handle per type descriptor
	mia_vector_of(uint8_t)            scratch                         ;   // plugin arguments in memory
	mia_vector_of(_mia_vm_prepared_s) prepared                        ;   // per function
	mia_vector_of(uint8_t*)           zeros                           ;   // the zero value of every type, made on first use (null: all zero bytes)
	mia_string_s                      buffer                          ;   // what an intrinsic prints
	mia_string_s                      bytes                           ;
	uint64_t                          steps                           ;   // instructions executed
	uint64_t                          allocations                     ;
	uint64_t                          call_depth                      ;   // the limit of nested calls
	uint64_t                          frame_bytes                     ;   // the byte areas of the running calls, together
	bool                              returned                        ;
	_mia_vm_word_u                    result                          ;
	// stopping
	jmp_buf* stop ;
	char*    error;
};


// runtime errors
/// stops the program with a runtime error.
mia_noreturn static void _mia_vm_fail(_mia_vm_s* const vm, const char* const format, ...) mia_printf_like(2, 3);
mia_noreturn static void _mia_vm_vfail(_mia_vm_s* const vm, const char* const format, va_list args) mia_printf_like(2, 0);

// memory
/// a new block of 'kind' over 'data' (the entry is reused when one is free).
static uint64_t _mia_vm_block_new(_mia_vm_s* const vm, const _mia_vm_block_kind_e kind, uint8_t* const data, const uint64_t size,
								  void (*release)(void* const memory));

/// ends a block: its handles stop working; heap memory is given back.
static void _mia_vm_block_end(_mia_vm_s* const vm, const uint64_t handle);

/// the memory of a live block; stops with the right message when it died.
static uint8_t* _mia_vm_resolve(_mia_vm_s* const vm, const uint64_t handle);

/// stops because 'handle' cannot be used: the block died, or the access
/// ('size' bytes at 'offset') is outside of it.
mia_noreturn static void _mia_vm_bad_access(_mia_vm_s* const vm, const uint64_t handle);

/// 'size' bytes at 'offset' of a live block (the fast path of every access).
static inline uint8_t* _mia_vm_memory(_mia_vm_s* const vm, const uint64_t handle, const uint64_t offset, const uint64_t size) {
	const _mia_vm_blocks_s* const table = &vm->blocks[handle >> 62];
	const uint32_t index = (uint32_t)handle;
	if (index < table->count) {
		const _mia_vm_block_s* const block = &table->items[index];
		if (block->generation == ((uint32_t)(handle >> 32) & _mia_vm_handle_generation_mask) && offset <= block->size && size <= block->size - offset) {
			return block->data + offset;
		}
	}
	_mia_vm_bad_access(vm, handle);
}

/// the entry of a handle, or null when the block died (or never existed).
static _mia_vm_block_s* _mia_vm_block_of(_mia_vm_s* const vm, const uint64_t handle);

/// the operand stack has room for 'count' more words.
static void _mia_vm_reserve(_mia_vm_s* const vm, const size_t count);

/// the byte area of a new frame / of the frame being left.
static uint8_t* _mia_vm_frame_alloc(_mia_vm_s* const vm, _mia_vm_frame_s* const frame, const uint64_t size);
static void _mia_vm_frame_release(_mia_vm_s* const vm, _mia_vm_frame_s* const frame);

/// the handle of a frame's block (made when first needed).
static uint64_t _mia_vm_frame_handle(_mia_vm_s* const vm, _mia_vm_frame_s* const frame);

/// words on the operand stack of a value of the type descriptor 'type'.
static uint64_t _mia_vm_type_words(const _mia_vm_s* const vm, const int64_t type);

/// words on the operand stack of a value of 'code' that is 'size' bytes.
static inline uint64_t _mia_vm_value_words(const mia_code_e code, const uint64_t size) {
	if (mia_code_bytes == code) return (size + 7) / 8;
	return (mia_code_slice == code) ? 2 : 1;
}

/// a scalar (or a function value) in memory, as a word, and back.
static inline _mia_vm_word_u _mia_vm_read(const mia_code_e code, const uint8_t* const memory) {
	_mia_vm_word_u word = { .u = 0, };
	switch (code) {
		case mia_code_s8:   word.i = (int8_t)memory[0]; break;
		case mia_code_u8:   word.u = memory[0]; break;
		case mia_code_bool: word.u = memory[0] != 0; break;
		case mia_code_s16:  { int16_t value; memcpy(&value, memory, 2); word.i = value; break; }
		case mia_code_u16:  { uint16_t value; memcpy(&value, memory, 2); word.u = value; break; }
		case mia_code_s32:  { int32_t value; memcpy(&value, memory, 4); word.i = value; break; }
		case mia_code_u32:  { uint32_t value; memcpy(&value, memory, 4); word.u = value; break; }
		case mia_code_f32:  { float value; memcpy(&value, memory, 4); word.f = (double)value; break; }
		default:           memcpy(&word, memory, 8); break;
	}
	return word;
}

static inline void _mia_vm_write(const mia_code_e code, uint8_t* const memory, const _mia_vm_word_u word) {
	switch (code) {
		case mia_code_s8: case mia_code_u8:   memory[0] = (uint8_t)word.u; break;
		case mia_code_bool:                  memory[0] = word.u != 0; break;
		case mia_code_s16: case mia_code_u16: { const uint16_t value = (uint16_t)word.u; memcpy(memory, &value, 2); break; }
		case mia_code_s32: case mia_code_u32: { const uint32_t value = (uint32_t)word.u; memcpy(memory, &value, 4); break; }
		case mia_code_f32:                   { const float value = (float)word.f; memcpy(memory, &value, 4); break; }
		default:                            memcpy(memory, &word, 8); break;
	}
}

/// the value of an integer word of 'code' as a 128-bit integer (for messages and checks).
static mia_int128_s _mia_vm_integer(const mia_code_e code, const _mia_vm_word_u word);

/// 'value' wrapped to the integer type 'code', as a word.
static _mia_vm_word_u _mia_vm_wrap(const mia_code_e code, const uint64_t value);

/// 'value' rounded as the float type 'code' (f32 rounds).
static double _mia_vm_round(const mia_code_e code, const double value);

/// 'value' in decimal (for messages).
static const char* _mia_vm_int_text(char buffer[48], const mia_int128_s value);

/// the constants of the module, made ready (fails on a broken module).
static void _mia_vm_load_constants(_mia_vm_s* const vm);

/// the handle of a constant's block (read-only).
static uint64_t _mia_vm_constant_handle(_mia_vm_s* const vm, const int64_t index);

/// the zero value of a type in memory (null: all zero bytes).
static const uint8_t* _mia_vm_zero(_mia_vm_s* const vm, const int64_t type);

static void _mia_vm_free_memory(_mia_vm_s* const vm);

// plugin calls
/// links every intrinsic of the module to its plugin function (stops when one is missing).
static void _mia_vm_link(_mia_vm_s* const vm, const mia_plugins_s* const plugins);
static void _mia_vm_unlink(_mia_vm_s* const vm);

/// calls intrinsic 'index': its arguments are the top 'words' words of the
/// stack, and its result replaces them.
static void _mia_vm_call_plugin(_mia_vm_s* const vm, const int64_t index, const int64_t words);

/// words of the arguments and of the result of an intrinsic.
static uint64_t _mia_vm_intrinsic_arguments(const _mia_vm_s* const vm, const mia_bytecode_intrinsic_s* const intrinsic);
static uint64_t _mia_vm_intrinsic_result(const _mia_vm_s* const vm, const mia_bytecode_intrinsic_s* const intrinsic);

// === memory =================================================================

/// the machine's memory: blocks and their handles, the byte areas of
/// frames, scalars in memory, and the images of constants.

#define _mia_vm_chunk_size ((size_t)1 << 20)

// --- blocks ------------------------------------------------------------------------------

static uint64_t _mia_vm_block_new(_mia_vm_s* const vm, const _mia_vm_block_kind_e kind, uint8_t* const data, const uint64_t size,
								  void (*release)(void* const memory)) {
	_mia_vm_blocks_s* const table = &vm->blocks[kind];
	uint32_t index = table->free;
	if (index != 0) {
		table->free = table->items[index].next_free;
	}
	else {
		if (0 == table->count) table->count = 1;   // index 0 is no block
		if (table->count >= table->capacity) {
			table->capacity = (table->capacity > 0) ? table->capacity * 2 : 64;
			table->items = mia_realloc(table->items, table->capacity * sizeof(*table->items));
		}
		index = table->count++;
		table->items[index] = (_mia_vm_block_s) { .generation = 1, };
	}
	_mia_vm_block_s* const block = &table->items[index];
	block->data = data;
	block->size = size;
	block->alive = true;
	block->next_free = 0;
	block->release = release;
	return _mia_vm_handle_make(kind, index, block->generation);
}

static void _mia_vm_block_end(_mia_vm_s* const vm, const uint64_t handle) {
	const _mia_vm_block_kind_e kind = _mia_vm_handle_kind(handle);
	_mia_vm_blocks_s* const table = &vm->blocks[kind];
	const uint32_t index = _mia_vm_handle_index(handle);
	if (_mia_vm_block_none == kind || index >= table->count) return;
	_mia_vm_block_s* const block = &table->items[index];
	if (!block->alive) return;
	if (block->release != NULL) block->release(block->data);
	block->alive = false;
	block->data = NULL;
	block->size = 0;
	block->release = NULL;
	block->generation = (block->generation + 1) & _mia_vm_handle_generation_mask;
	if (0 == block->generation) block->generation = 1;
	block->next_free = table->free;
	table->free = index;
}

static _mia_vm_block_s* _mia_vm_block_of(_mia_vm_s* const vm, const uint64_t handle) {
	const _mia_vm_block_kind_e kind = _mia_vm_handle_kind(handle);
	_mia_vm_blocks_s* const table = &vm->blocks[kind];
	const uint32_t index = _mia_vm_handle_index(handle);
	if (_mia_vm_block_none == kind || 0 == index || index >= table->count) return NULL;
	_mia_vm_block_s* const block = &table->items[index];
	if (!block->alive || block->generation != _mia_vm_handle_generation(handle)) return NULL;
	return block;
}

static void _mia_vm_bad_access(_mia_vm_s* const vm, const uint64_t handle) {
	if (_mia_vm_block_of(vm, handle) != NULL) _mia_vm_fail(vm, "an access outside of its memory (the module is broken)");
	switch (_mia_vm_handle_kind(handle)) {
		case _mia_vm_block_heap:  _mia_vm_fail(vm, "use of freed heap memory");
		case _mia_vm_block_frame: _mia_vm_fail(vm, "use of a local array of a function that has returned");
		default:             _mia_vm_fail(vm, "use of memory that does not exist");
	}
}

static uint8_t* _mia_vm_resolve(_mia_vm_s* const vm, const uint64_t handle) {
	_mia_vm_block_s* const block = _mia_vm_block_of(vm, handle);
	if (block != NULL) return block->data;
	_mia_vm_bad_access(vm, handle);
}

/// 'size' zeroed bytes for what a program's sizes decide; a runtime error, not a panic, when the system
/// has not that much.
static void* _mia_vm_allocate(_mia_vm_s* const vm, const uint64_t size) {
	void* const memory = (size <= SIZE_MAX) ? calloc(1, size > 0 ? (size_t)size : 1) : NULL;
	if (NULL == memory) _mia_vm_fail(vm, "out of memory: the program needs %llu more bytes", (unsigned long long)size);
	return memory;
}

static void _mia_vm_reserve(_mia_vm_s* const vm, const size_t count) {
	if (vm->capacity - vm->sp >= count) return;
	size_t capacity = (vm->capacity > 0) ? vm->capacity : 1024;
	while (capacity - vm->sp < count) capacity *= 2;
	_mia_vm_word_u* const stack = (capacity <= SIZE_MAX / sizeof(*vm->stack)) ? realloc(vm->stack, capacity * sizeof(*vm->stack)) : NULL;
	if (NULL == stack) _mia_vm_fail(vm, "out of memory: the operand stack needs %llu words", (unsigned long long)capacity);
	vm->stack = stack;
	vm->capacity = capacity;
}

// --- frames ------------------------------------------------------------------------------

static uint8_t* _mia_vm_frame_alloc(_mia_vm_s* const vm, _mia_vm_frame_s* const frame, const uint64_t size) {
	const size_t rounded = (size_t)((size + 15) / 16 * 16);
	if (0 == vm->chunks.count) {
		const _mia_vm_chunk_s chunk = { .data = _mia_vm_allocate(vm, _mia_vm_chunk_size), .size = _mia_vm_chunk_size, };
		mia_vector_push(&vm->chunks, chunk);
		vm->chunk = 0;
	}
	_mia_vm_chunk_s* chunk = &vm->chunks.data[vm->chunk];
	while (chunk->size - chunk->used < rounded) {
		// the next chunk (chunks never move, so pointers into frames stay valid)
		if (vm->chunk + 1 < vm->chunks.count && vm->chunks.data[vm->chunk + 1].size >= rounded) {
			vm->chunk += 1;
			vm->chunks.data[vm->chunk].used = 0;
		}
		else {
			const size_t size_needed = (rounded > _mia_vm_chunk_size) ? rounded : _mia_vm_chunk_size;
			const _mia_vm_chunk_s fresh = { .data = _mia_vm_allocate(vm, size_needed), .size = size_needed, };
			// a chunk too small to come back to is replaced
			if (vm->chunk + 1 < vm->chunks.count) {
				mia_free(vm->chunks.data[vm->chunk + 1].data);
				vm->chunks.data[vm->chunk + 1] = fresh;
			}
			else {
				mia_vector_push(&vm->chunks, fresh);
			}
			vm->chunk += 1;
		}
		chunk = &vm->chunks.data[vm->chunk];
	}
	frame->chunk = vm->chunk;
	frame->used = chunk->used;
	uint8_t* const base = chunk->data + chunk->used;
	chunk->used += rounded;
	memset(base, 0, rounded);
	return base;
}

static void _mia_vm_frame_release(_mia_vm_s* const vm, _mia_vm_frame_s* const frame) {
	vm->frame_bytes -= vm->prepared.data[frame->function].frame;
	if (frame->handle != 0) _mia_vm_block_end(vm, frame->handle);
	frame->handle = 0;
	vm->chunk = frame->chunk;
	vm->chunks.data[frame->chunk].used = frame->used;
}

static uint64_t _mia_vm_frame_handle(_mia_vm_s* const vm, _mia_vm_frame_s* const frame) {
	if (0 == frame->handle) {
		const mia_bytecode_function_s* const function = &vm->module->functions.data[frame->function];
		frame->handle = _mia_vm_block_new(vm, _mia_vm_block_frame, frame->base, function->frame, NULL);
	}
	return frame->handle;
}

// --- scalars -----------------------------------------------------------------------------

static uint64_t _mia_vm_type_words(const _mia_vm_s* const vm, const int64_t type) {
	const mia_bytecode_type_s* const descriptor = &vm->module->types.data[type];
	return _mia_vm_value_words(descriptor->code, descriptor->size);
}

static mia_int128_s _mia_vm_integer(const mia_code_e code, const _mia_vm_word_u word) {
	return mia_code_is_signed(code) ? mia_int128_from_int(word.i) : mia_int128_from_uint(word.u);
}

static _mia_vm_word_u _mia_vm_wrap(const mia_code_e code, const uint64_t value) {
	_mia_vm_word_u word = { .u = value, };
	switch (code) {
		case mia_code_s8:  word.i = (int8_t)(uint8_t)value; break;
		case mia_code_u8:  word.u = (uint8_t)value; break;
		case mia_code_s16: word.i = (int16_t)(uint16_t)value; break;
		case mia_code_u16: word.u = (uint16_t)value; break;
		case mia_code_s32: word.i = (int32_t)(uint32_t)value; break;
		case mia_code_u32: word.u = (uint32_t)value; break;
		case mia_code_bool: word.u = value != 0; break;
		default: break;
	}
	return word;
}

static double _mia_vm_round(const mia_code_e code, const double value) {
	if (mia_code_f32 == code && isfinite(value)) return (double)(float)value;
	return value;
}

static const char* _mia_vm_int_text(char buffer[48], const mia_int128_s value) {
	return mia_int128_text(buffer, value);
}

// --- constants ---------------------------------------------------------------------------

/// the word of a scalar constant.
static _mia_vm_word_u _mia_vm_scalar_word(const mia_bytecode_constant_s* const constant) {
	_mia_vm_word_u word = { .u = 0, };
	switch (constant->kind) {
		case mia_bytecode_constant_integer:   word.u = constant->integer; break;
		case mia_bytecode_constant_real:      word.f = constant->real; break;
		case mia_bytecode_constant_boolean:   word.u = constant->boolean; break;
		case mia_bytecode_constant_function:  word.u = constant->integer; break;
		case mia_bytecode_constant_intrinsic: word.u = constant->integer | ((uint64_t)1 << 63); break;
		default: break;
	}
	return word;
}

static void _mia_vm_image(_mia_vm_s* const vm, const mia_bytecode_constant_s* const constant, const int64_t type, uint8_t* const memory, const int depth);

/// a read-only static block holding the items of a constant slice.
static uint64_t _mia_vm_static_items(_mia_vm_s* const vm, const mia_bytecode_constant_s* const constant, const int64_t element, const int depth) {
	const mia_bytecode_type_s* const descriptor = &vm->module->types.data[element];
	const uint64_t size = descriptor->size * constant->items.count;
	uint8_t* const data = _mia_vm_allocate(vm, size);
	// the block owns the memory before it is filled: filling may stop the machine
	const uint64_t handle = _mia_vm_block_new(vm, _mia_vm_block_static, data, size, mia_free) | _mia_vm_handle_read_only;
	for (size_t i = 0; i < constant->items.count; ++i) _mia_vm_image(vm, &constant->items.data[i], element, data + i * descriptor->size, depth + 1);
	return handle;
}

/// writes the memory image of 'constant' as a value of 'type'.
static void _mia_vm_image(_mia_vm_s* const vm, const mia_bytecode_constant_s* const constant, const int64_t type, uint8_t* const memory, const int depth) {
	const mia_bytecode_module_s* const module = vm->module;
	if (type < 0 || type >= (int64_t)module->types.count || depth > mia_bytecode_nesting_limit) _mia_vm_fail(vm, "the module is broken (a constant has no type)");
	const mia_bytecode_type_s* const descriptor = &module->types.data[type];
	switch (descriptor->kind) {
		case mia_bytecode_descriptor_int: case mia_bytecode_descriptor_float: case mia_bytecode_descriptor_bool: case mia_bytecode_descriptor_function:
			_mia_vm_write(descriptor->code, memory, _mia_vm_scalar_word(constant));
			return;
		case mia_bytecode_descriptor_array: {
			const mia_bytecode_type_s* const element = &module->types.data[descriptor->element];
			if (constant->kind != mia_bytecode_constant_list || (int64_t)constant->items.count != descriptor->length
					|| element->size * (uint64_t)descriptor->length > descriptor->size) {
				_mia_vm_fail(vm, "the module is broken (a constant does not fit its type)");
			}
			for (size_t i = 0; i < constant->items.count; ++i) _mia_vm_image(vm, &constant->items.data[i], descriptor->element, memory + i * element->size, depth + 1);
			return;
		}
		case mia_bytecode_descriptor_struct:
			if (constant->kind != mia_bytecode_constant_list || constant->items.count != descriptor->field_types.count) {
				_mia_vm_fail(vm, "the module is broken (a constant does not fit its type)");
			}
			for (size_t i = 0; i < constant->items.count; ++i) {
				const int64_t field = descriptor->field_types.data[i];
				_mia_vm_image(vm, &constant->items.data[i], field, memory + descriptor->field_offsets.data[i], depth + 1);
			}
			return;
		case mia_bytecode_descriptor_slice: {
			if (constant->kind != mia_bytecode_constant_slice) _mia_vm_fail(vm, "the module is broken (a constant does not fit its type)");
			memset(memory, 0, 16);
			if (constant->null) return;
			const uint64_t element = module->types.data[descriptor->element].size;
			const int64_t count = (int64_t)constant->items.count;
			if (constant->start < 0 || constant->length < 0 || constant->start > count || constant->length > count - constant->start) {
				_mia_vm_fail(vm, "the module is broken (a constant slice is out of bounds)");
			}
			uint64_t handle = _mia_vm_static_items(vm, constant, descriptor->element, depth);
			if (!constant->read_only) handle &= ~_mia_vm_handle_read_only;
			const uint64_t words[2] = { handle, _mia_vm_slice_word((uint64_t)constant->start * element, (uint64_t)constant->length), };
			memcpy(memory, words, 16);
			return;
		}
		default:
			_mia_vm_fail(vm, "the module is broken (an unknown type)");
	}
}

/// checks a type descriptor: values of it are read and written by its
/// layout (constants, plugins), so the layout must fit in its size.
static void _mia_vm_check_type(_mia_vm_s* const vm, const mia_bytecode_type_s* const type) {
	const mia_bytecode_module_s* const module = vm->module;
	if (type->size > mia_bytecode_type_size_limit) _mia_vm_fail(vm, "the module is broken (a type is too large)");
	const char kind = mia_code_kind(type->code);
	bool good = true;
	switch (type->kind) {
		case mia_bytecode_descriptor_int:      good = 'i' == kind && mia_code_size(type->code) == type->size; break;
		case mia_bytecode_descriptor_float:    good = 'f' == kind && mia_code_size(type->code) == type->size; break;
		case mia_bytecode_descriptor_bool:     good = 'b' == kind && mia_code_size(type->code) == type->size; break;
		case mia_bytecode_descriptor_function: good = 'n' == kind && mia_code_size(type->code) == type->size; break;
		case mia_bytecode_descriptor_slice:    good = mia_code_slice == type->code && 16 == type->size; break;
		case mia_bytecode_descriptor_array: {
			const uint64_t element = module->types.data[type->element].size;
			good = mia_code_bytes == type->code && type->length >= 0
				&& (0 == element || (uint64_t)type->length <= type->size / element);
			break;
		}
		case mia_bytecode_descriptor_struct:
			good = mia_code_bytes == type->code && type->field_offsets.count == type->field_types.count;
			for (size_t f = 0; good && f < type->field_types.count; ++f) {
				const uint64_t offset = type->field_offsets.data[f], size = module->types.data[type->field_types.data[f]].size;
				good = offset <= type->size && size <= type->size - offset;
			}
			break;
		default: good = false; break;
	}
	if (!good) _mia_vm_fail(vm, "the module is broken (the type `%s` does not fit its size)", type->name);
}

static void _mia_vm_load_constants(_mia_vm_s* const vm) {
	const mia_bytecode_module_s* const module = vm->module;
	for (size_t i = 0; i < module->types.count; ++i) _mia_vm_check_type(vm, &module->types.data[i]);
	for (size_t i = 0; i < module->constants.count; ++i) {
		const mia_bytecode_constant_s* const constant = &module->constants.data[i];
		_mia_vm_constant_s ready = {0};
		if (constant->type >= 0) {
			const uint64_t size = module->types.data[constant->type].size;
			ready.count = (size_t)((size + 7) / 8);
			ready.words = _mia_vm_allocate(vm, (uint64_t)ready.count * sizeof(_mia_vm_word_u));
			// kept before it is filled: filling may stop the machine
			mia_vector_push(&vm->constants, ready);
			_mia_vm_image(vm, constant, constant->type, (uint8_t*)ready.words, 0);
			continue;
		}
		else if (mia_bytecode_constant_slice == constant->kind) {
			ready.count = 2;
			ready.words = mia_calloc(2, sizeof(_mia_vm_word_u));
		}
		else if (constant->kind != mia_bytecode_constant_none && constant->kind != mia_bytecode_constant_text) {
			ready.count = 1;
			ready.words = mia_calloc(1, sizeof(_mia_vm_word_u));
			ready.words[0] = _mia_vm_scalar_word(constant);
		}
		mia_vector_push(&vm->constants, ready);
	}
}

static uint64_t _mia_vm_constant_handle(_mia_vm_s* const vm, const int64_t index) {
	_mia_vm_constant_s* const constant = &vm->constants.data[index];
	if (0 == constant->handle) {
		const int64_t type = vm->module->constants.data[index].type;
		const uint64_t size = (type >= 0) ? vm->module->types.data[type].size : constant->count * 8;
		constant->handle = _mia_vm_block_new(vm, _mia_vm_block_static, (uint8_t*)constant->words, size, NULL) | _mia_vm_handle_read_only;
	}
	return constant->handle;
}

static const uint8_t* _mia_vm_zero(_mia_vm_s* const vm, const int64_t type) {
	const int64_t zero = vm->module->types.data[type].zero;
	if (zero < 0) return NULL;
	return (const uint8_t*)vm->constants.data[zero].words;
}

static void _mia_vm_free_memory(_mia_vm_s* const vm) {
	for (int kind = 0; kind < _mia_vm_block_kind_count; ++kind) {
		_mia_vm_blocks_s* const table = &vm->blocks[kind];
		for (uint32_t i = 1; i < table->count; ++i) {
			_mia_vm_block_s* const block = &table->items[i];
			if (block->alive && block->release != NULL) block->release(block->data);
		}
		mia_free(table->items);
		*table = (_mia_vm_blocks_s) {0};
	}
	for (size_t i = 0; i < vm->constants.count; ++i) mia_free(vm->constants.data[i].words);
	mia_vector_free(&vm->constants);
	for (size_t i = 0; i < vm->chunks.count; ++i) mia_free(vm->chunks.data[i].data);
	mia_vector_free(&vm->chunks);
	mia_free(vm->stack);
	vm->stack = NULL;
}

// === plugin calls ===========================================================

/// calls of plugin functions at run time: linking intrinsics to the plugin
/// functions that provide them, and the run-time half of the plugin api
/// (see mia/plugin.h). a call handle points at the arguments where they are,
/// on the operand stack; nothing is copied unless a plugin asks for the
/// memory of a scalar.

typedef struct {
	mia_plugin_call_s     is_public;   // first: the handle the plugin gets
	_mia_vm_s*            vm       ;
	const _mia_vm_link_s* link     ;
	const _mia_vm_word_u* arguments;
} _mia_vm_call_s;

#define _mia_vm_value_none  0
#define _mia_vm_value_word  1
#define _mia_vm_value_slice 2

static _mia_vm_call_s* _mia_vm_call_of(mia_plugin_call_s* const call) {
	return (_mia_vm_call_s*)call;
}

// --- linking -----------------------------------------------------------------------------

static uint64_t _mia_vm_intrinsic_arguments(const _mia_vm_s* const vm, const mia_bytecode_intrinsic_s* const intrinsic) {
	uint64_t words = 0;
	for (size_t i = 0; i < intrinsic->parameters.count; ++i) words += _mia_vm_type_words(vm, intrinsic->parameters.data[i]);
	for (size_t i = 0; i < intrinsic->variadic.count; ++i) words += _mia_vm_type_words(vm, intrinsic->variadic.data[i]);
	return words;
}

static uint64_t _mia_vm_intrinsic_result(const _mia_vm_s* const vm, const mia_bytecode_intrinsic_s* const intrinsic) {
	return (intrinsic->result >= 0) ? _mia_vm_type_words(vm, intrinsic->result) : 0;
}

static void _mia_vm_link(_mia_vm_s* const vm, const mia_plugins_s* const plugins) {
	const mia_bytecode_module_s* const module = vm->module;
	vm->types = mia_plugins_types_new(module);
	for (size_t i = 0; i < module->intrinsics.count; ++i) {
		const mia_bytecode_intrinsic_s* const intrinsic = &module->intrinsics.data[i];
		_mia_vm_link_s link = { .intrinsic = intrinsic, .function = mia_plugins_find(plugins, intrinsic->name), };
		link.fixed = intrinsic->parameters.count;
		link.count = link.fixed + intrinsic->variadic.count;
		link.types = mia_calloc(link.count + 1, sizeof(*link.types));
		link.offsets = mia_calloc(link.count + 1, sizeof(*link.offsets));
		size_t offset = 0;
		for (size_t a = 0; a < link.count; ++a) {
			const int64_t type = (a < link.fixed) ? intrinsic->parameters.data[a] : intrinsic->variadic.data[a - link.fixed];
			link.types[a] = &vm->types[type];
			link.offsets[a] = offset;
			offset += (size_t)_mia_vm_type_words(vm, type);
		}
		link.result = (intrinsic->result >= 0) ? &vm->types[intrinsic->result] : NULL;
		link.result_words = _mia_vm_intrinsic_result(vm, intrinsic);
		mia_vector_push(&vm->links, link);
		if (NULL == link.function) _mia_vm_fail(vm, "no plugin provides `%s`", intrinsic->name);
	}
}

static void _mia_vm_unlink(_mia_vm_s* const vm) {
	for (size_t i = 0; i < vm->links.count; ++i) {
		mia_free((void*)(uintptr_t)vm->links.data[i].types);
		mia_free(vm->links.data[i].offsets);
	}
	mia_vector_free(&vm->links);
	mia_plugins_types_free(vm->types, vm->module->types.count);
	vm->types = NULL;
	mia_vector_free(&vm->scratch);
}

static void _mia_vm_call_plugin(_mia_vm_s* const vm, const int64_t index, const int64_t words) {
	const _mia_vm_link_s* const link = &vm->links.data[index];
	_mia_vm_call_s call = { .is_public = { .interface = mia_plugins_table(), }, .vm = vm, .link = link, .arguments = vm->stack + vm->sp - words, };
	if (vm->scratch.capacity < 8 * link->count) mia_vector_reserve(&vm->scratch, 8 * link->count);
	const mia_plugin_value_s value = link->function->run(&call.is_public);
	const uint64_t given = (_mia_vm_value_slice == value.kind) ? 2 : (_mia_vm_value_word == value.kind) ? 1 : 0;
	if (given != link->result_words || (2 == given && link->result != NULL && link->result->kind != mia_plugin_kind_slice)) {
		_mia_vm_fail(vm, "the plugin function `%s` returned %s", link->intrinsic->name, (0 == given) ? "no value" : "a value of another kind");
	}
	vm->sp -= (size_t)words;
	for (uint64_t i = 0; i < given; ++i) vm->stack[vm->sp++].u = value.words[i];
}

// --- arguments ---------------------------------------------------------------------------

/// the words of argument 'index' (stops when there is no such argument).
static const _mia_vm_word_u* _mia_vm_argument(_mia_vm_call_s* const call, const size_t index) {
	if (index >= call->link->count) {
		_mia_vm_fail(call->vm, "the plugin function `%s` reads argument %zu, but it has %zu", call->link->intrinsic->name, index, call->link->count);
	}
	return call->arguments + call->link->offsets[index];
}

static const mia_plugin_type_s* _mia_vm_argument_type(_mia_vm_call_s* const call, const size_t index) {
	(void)_mia_vm_argument(call, index);
	return call->link->types[index];
}

/// a scalar argument (any number or bool) as a word, and its type code.
static _mia_vm_word_u _mia_vm_scalar(_mia_vm_call_s* const call, const size_t index, mia_code_e* const code) {
	const _mia_vm_word_u* const words = _mia_vm_argument(call, index);
	const mia_plugin_type_s* const type = call->link->types[index];
	if (type->kind != mia_plugin_kind_int && type->kind != mia_plugin_kind_float && type->kind != mia_plugin_kind_bool) {
		_mia_vm_fail(call->vm, "the plugin function `%s` reads argument %zu (a `%s`) as a number", call->link->intrinsic->name, index, type->name);
	}
	*code = type->code;
	return words[0];
}

/// a double as a 64-bit integer, saturated (c leaves out-of-range conversions undefined).
static int64_t _mia_vm_to_int(const double value) {
	if (isnan(value)) return 0;
	if (value >= 9223372036854775807.0) return INT64_MAX;
	if (value <= -9223372036854775808.0) return INT64_MIN;
	return (int64_t)value;
}

static size_t _mia_vm_plugin_count(mia_plugin_call_s* const call) {
	return _mia_vm_call_of(call)->link->count;
}

static size_t _mia_vm_plugin_fixed_count(mia_plugin_call_s* const call) {
	return _mia_vm_call_of(call)->link->fixed;
}

static const mia_plugin_type_s* _mia_vm_plugin_get_argument_type(mia_plugin_call_s* const call, const size_t index) {
	return _mia_vm_argument_type(_mia_vm_call_of(call), index);
}

static size_t _mia_vm_plugin_type_argument_count(mia_plugin_call_s* const call) {
	return _mia_vm_call_of(call)->link->intrinsic->arguments.count;
}

static const mia_plugin_type_s* _mia_vm_plugin_type_argument(mia_plugin_call_s* const call, const size_t index) {
	_mia_vm_call_s* const self = _mia_vm_call_of(call);
	const mia_bytecode_indexes_s* const arguments = &self->link->intrinsic->arguments;
	if (index >= arguments->count) {
		_mia_vm_fail(self->vm, "the plugin function `%s` reads type argument %zu, but it has %zu", self->link->intrinsic->name, index, arguments->count);
	}
	return &self->vm->types[arguments->data[index]];
}

static uint32_t _mia_vm_plugin_argument_flags(mia_plugin_call_s* const call, const size_t index) {
	const mia_bytecode_intrinsic_s* const intrinsic = _mia_vm_call_of(call)->link->intrinsic;
	return (index < intrinsic->flags.count) ? intrinsic->flags.data[index] : 0;
}

static int64_t _mia_vm_plugin_get_int(mia_plugin_call_s* const call, const size_t index) {
	mia_code_e code = mia_code_int;
	const _mia_vm_word_u word = _mia_vm_scalar(_mia_vm_call_of(call), index, &code);
	return mia_code_is_float(code) ? _mia_vm_to_int(word.f) : word.i;
}

static uint64_t _mia_vm_plugin_get_uint(mia_plugin_call_s* const call, const size_t index) {
	mia_code_e code = mia_code_int;
	const _mia_vm_word_u word = _mia_vm_scalar(_mia_vm_call_of(call), index, &code);
	return mia_code_is_float(code) ? (uint64_t)_mia_vm_to_int(word.f) : word.u;
}

static double _mia_vm_plugin_get_float(mia_plugin_call_s* const call, const size_t index) {
	mia_code_e code = mia_code_int;
	const _mia_vm_word_u word = _mia_vm_scalar(_mia_vm_call_of(call), index, &code);
	if (mia_code_is_float(code)) return word.f;
	return mia_code_is_signed(code) ? (double)word.i : (double)word.u;
}

static bool _mia_vm_plugin_get_bool(mia_plugin_call_s* const call, const size_t index) {
	mia_code_e code = mia_code_int;
	const _mia_vm_word_u word = _mia_vm_scalar(_mia_vm_call_of(call), index, &code);
	return mia_code_is_float(code) ? word.f != 0.0 : word.u != 0;
}

static const void* _mia_vm_plugin_get_memory(mia_plugin_call_s* const call, const size_t index) {
	_mia_vm_call_s* const self = _mia_vm_call_of(call);
	const _mia_vm_word_u* const words = _mia_vm_argument(self, index);
	const mia_plugin_type_s* const type = self->link->types[index];
	if (type->code < mia_code_slice) {
		// a scalar is a word on the stack: written as it is in memory
		uint8_t* const memory = self->vm->scratch.data + 8 * index;
		_mia_vm_write(type->code, memory, words[0]);
		return memory;
	}
	return words;
}

/// a slice from its two words.
static mia_plugin_slice_s _mia_vm_slice(_mia_vm_call_s* const call, const uint64_t handle, const uint64_t word, const mia_plugin_type_s* const type) {
	mia_plugin_slice_s slice = { .length = _mia_vm_slice_length(word), .element = type->element, .read_only = (handle & _mia_vm_handle_read_only) != 0, };
	if (0 == (handle & ~_mia_vm_handle_read_only)) {
		slice.length = 0;
		return slice;
	}
	const uint64_t size = (type->element != NULL) ? type->element->size : 1;
	slice.data = _mia_vm_memory(call->vm, handle, _mia_vm_slice_offset(word), slice.length * size);
	return slice;
}

static const _mia_vm_word_u* _mia_vm_slice_argument(_mia_vm_call_s* const call, const size_t index) {
	const _mia_vm_word_u* const words = _mia_vm_argument(call, index);
	if (call->link->types[index]->kind != mia_plugin_kind_slice) {
		_mia_vm_fail(call->vm, "the plugin function `%s` reads argument %zu (a `%s`) as a slice", call->link->intrinsic->name, index,
			call->link->types[index]->name);
	}
	return words;
}

static mia_plugin_slice_s _mia_vm_plugin_get_slice(mia_plugin_call_s* const call, const size_t index) {
	_mia_vm_call_s* const self = _mia_vm_call_of(call);
	const _mia_vm_word_u* const words = _mia_vm_slice_argument(self, index);
	return _mia_vm_slice(self, words[0].u, words[1].u, self->link->types[index]);
}

static uint64_t _mia_vm_plugin_slice_length(mia_plugin_call_s* const call, const size_t index) {
	return _mia_vm_slice_length(_mia_vm_slice_argument(_mia_vm_call_of(call), index)[1].u);
}

static bool _mia_vm_plugin_slice_read_only(mia_plugin_call_s* const call, const size_t index) {
	return (_mia_vm_slice_argument(_mia_vm_call_of(call), index)[0].u & _mia_vm_handle_read_only) != 0;
}

static mia_plugin_slice_s _mia_vm_plugin_slice_at(mia_plugin_call_s* const call, const void* const memory, const mia_plugin_type_s* const type) {
	uint64_t words[2];
	memcpy(words, memory, sizeof(words));
	return _mia_vm_slice(_mia_vm_call_of(call), words[0], words[1], type);
}

static const char* _mia_vm_plugin_function_name_at(mia_plugin_call_s* const call, const void* const memory) {
	const mia_bytecode_module_s* const module = _mia_vm_call_of(call)->vm->module;
	uint64_t word = 0;
	memcpy(&word, memory, sizeof(word));
	const uint64_t index = word & ~((uint64_t)1 << 63);
	if (word >> 63) {
		if (index >= module->intrinsics.count) return "?";
		// plugin functions show their own name, without the module
		const char* const name = module->intrinsics.data[index].name;
		const char* const dot = strrchr(name, '.');
		return (dot != NULL) ? dot + 1 : name;
	}
	return (index < module->functions.count) ? module->functions.data[index].name : "?";
}

// --- results -----------------------------------------------------------------------------

/// a word of the declared result type.
static mia_plugin_value_s _mia_vm_result(_mia_vm_call_s* const call, const bool real, const double number, const uint64_t integer) {
	const mia_plugin_type_s* const type = call->link->result;
	mia_plugin_value_s value = { .kind = _mia_vm_value_word, };
	if (NULL == type || (type->kind != mia_plugin_kind_int && type->kind != mia_plugin_kind_float && type->kind != mia_plugin_kind_bool)) {
		_mia_vm_fail(call->vm, "the plugin function `%s` returned a number, but its result is not one", call->link->intrinsic->name);
	}
	_mia_vm_word_u word = { .u = 0, };
	if (mia_plugin_kind_float == type->kind) word.f = _mia_vm_round(type->code, real ? number : (double)(int64_t)integer);
	else if (mia_plugin_kind_bool == type->kind) word.u = real ? number != 0.0 : integer != 0;
	else word = _mia_vm_wrap(type->code, real ? (uint64_t)_mia_vm_to_int(number) : integer);
	value.words[0] = word.u;
	return value;
}

static mia_plugin_value_s _mia_vm_plugin_make_int(mia_plugin_call_s* const call, const int64_t value) {
	return _mia_vm_result(_mia_vm_call_of(call), false, 0.0, (uint64_t)value);
}

static mia_plugin_value_s _mia_vm_plugin_make_uint(mia_plugin_call_s* const call, const uint64_t value) {
	_mia_vm_call_s* const self = _mia_vm_call_of(call);
	if (self->link->result != NULL && mia_plugin_kind_float == self->link->result->kind) return _mia_vm_result(self, true, (double)value, 0);
	return _mia_vm_result(self, false, 0.0, value);
}

static mia_plugin_value_s _mia_vm_plugin_make_float(mia_plugin_call_s* const call, const double value) {
	return _mia_vm_result(_mia_vm_call_of(call), true, value, 0);
}

static mia_plugin_value_s _mia_vm_plugin_make_bool(mia_plugin_call_s* const call, const bool value) {
	return _mia_vm_result(_mia_vm_call_of(call), false, 0.0, value ? 1 : 0);
}

static mia_plugin_value_s _mia_vm_plugin_make_none(mia_plugin_call_s* const call) {
	mia_unused(call);
	return (mia_plugin_value_s) { .kind = _mia_vm_value_none, };
}

static mia_plugin_value_s _mia_vm_plugin_make_empty(mia_plugin_call_s* const call) {
	mia_unused(call);
	return (mia_plugin_value_s) { .kind = _mia_vm_value_slice, };
}

// --- heap memory -------------------------------------------------------------------------

#define _mia_vm_allocation_limit ((uint64_t)1 << 28)

static void _mia_vm_release(void* const memory) {
	free(memory);
}

static mia_plugin_value_s _mia_vm_heap_slice(_mia_vm_call_s* const call, const uint64_t count, uint8_t* const data, const uint64_t size,
											 void (*release)(void* const memory)) {
	call->vm->allocations += 1;
	const mia_plugin_value_s value = { .kind = _mia_vm_value_slice, .words = {
		_mia_vm_block_new(call->vm, _mia_vm_block_heap, data, size, release), _mia_vm_slice_word(0, count),
	}, };
	return value;
}

static void _mia_vm_check_count(_mia_vm_call_s* const call, const mia_plugin_type_s* const element, const uint64_t count) {
	if (count > _mia_vm_allocation_limit || (element->size > 0 && count > ((uint64_t)1 << 32) / element->size)) {
		_mia_vm_fail(call->vm, "cannot allocate %" PRIu64 " elements", count);
	}
}

static mia_plugin_value_s _mia_vm_plugin_heap_alloc(mia_plugin_call_s* const call, const mia_plugin_type_s* const element, const uint64_t count,
													void** const data) {
	_mia_vm_call_s* const self = _mia_vm_call_of(call);
	_mia_vm_check_count(self, element, count);
	const uint64_t size = count * element->size;
	uint8_t* const memory = calloc(1, size > 0 ? (size_t)size : 1);
	if (NULL == memory) _mia_vm_fail(self->vm, "cannot allocate %" PRIu64 " elements", count);
	const uint8_t* const zero = (element->index >= 0) ? _mia_vm_zero(self->vm, element->index) : NULL;
	if (zero != NULL) for (uint64_t i = 0; i < count; ++i) memcpy(memory + i * element->size, zero, (size_t)element->size);
	if (data != NULL) *data = memory;
	return _mia_vm_heap_slice(self, count, memory, size, _mia_vm_release);
}

static mia_plugin_value_s _mia_vm_plugin_heap_adopt(mia_plugin_call_s* const call, const mia_plugin_type_s* const element, const uint64_t count, void* const memory,
													void (*release)(void* const memory)) {
	_mia_vm_call_s* const self = _mia_vm_call_of(call);
	_mia_vm_check_count(self, element, count);
	if (NULL == memory) _mia_vm_fail(self->vm, "the plugin function `%s` gave no memory", self->link->intrinsic->name);
	return _mia_vm_heap_slice(self, count, memory, count * element->size, release);
}

static mia_plugin_heap_e _mia_vm_plugin_heap_of(mia_plugin_call_s* const call, const size_t index) {
	_mia_vm_call_s* const self = _mia_vm_call_of(call);
	const _mia_vm_word_u* const words = _mia_vm_slice_argument(self, index);
	const uint64_t handle = words[0].u & ~_mia_vm_handle_read_only;
	if (0 == handle) return mia_plugin_heap_none;
	if (_mia_vm_handle_kind(handle) != _mia_vm_block_heap) return mia_plugin_heap_other;
	(void)_mia_vm_resolve(self->vm, handle);
	const _mia_vm_block_s* const block = _mia_vm_block_of(self->vm, handle);
	const mia_plugin_type_s* const element = self->link->types[index]->element;
	const uint64_t size = (element != NULL) ? element->size : 1;
	const bool whole = 0 == _mia_vm_slice_offset(words[1].u) && _mia_vm_slice_length(words[1].u) * size == block->size;
	return whole ? mia_plugin_heap_whole : mia_plugin_heap_part;
}

static void _mia_vm_plugin_heap_free(mia_plugin_call_s* const call, const size_t index) {
	_mia_vm_call_s* const self = _mia_vm_call_of(call);
	if (_mia_vm_plugin_heap_of(call, index) != mia_plugin_heap_whole) {
		_mia_vm_fail(self->vm, "the plugin function `%s` frees memory that is not a whole heap allocation", self->link->intrinsic->name);
	}
	_mia_vm_block_end(self->vm, _mia_vm_argument(self, index)[0].u & ~_mia_vm_handle_read_only);
}

// --- the rest ----------------------------------------------------------------------------

static void _mia_vm_plugin_write(mia_plugin_call_s* const call, const char* const data, const size_t length) {
	if (length > 0) (void)fwrite(data, 1, length, _mia_vm_call_of(call)->vm->out);
}

static void _mia_vm_plugin_fail(mia_plugin_call_s* const call, const char* const format, va_list args) mia_printf_like(2, 0);

static void _mia_vm_plugin_fail(mia_plugin_call_s* const call, const char* const format, va_list args) {
	_mia_vm_vfail(_mia_vm_call_of(call)->vm, format, args);
}

void mia_vm_plugin_table(mia_plugin_interface_s* const table) {
	table->count = _mia_vm_plugin_count;
	table->fixed_count = _mia_vm_plugin_fixed_count;
	table->argument_type = _mia_vm_plugin_get_argument_type;
	table->type_argument_count = _mia_vm_plugin_type_argument_count;
	table->type_argument = _mia_vm_plugin_type_argument;
	table->argument_flags = _mia_vm_plugin_argument_flags;
	table->get_int = _mia_vm_plugin_get_int;
	table->get_uint = _mia_vm_plugin_get_uint;
	table->get_float = _mia_vm_plugin_get_float;
	table->get_bool = _mia_vm_plugin_get_bool;
	table->get_memory = _mia_vm_plugin_get_memory;
	table->get_slice = _mia_vm_plugin_get_slice;
	table->slice_length = _mia_vm_plugin_slice_length;
	table->slice_read_only = _mia_vm_plugin_slice_read_only;
	table->slice_at = _mia_vm_plugin_slice_at;
	table->function_name_at = _mia_vm_plugin_function_name_at;
	table->make_int = _mia_vm_plugin_make_int;
	table->make_uint = _mia_vm_plugin_make_uint;
	table->make_float = _mia_vm_plugin_make_float;
	table->make_bool = _mia_vm_plugin_make_bool;
	table->make_none = _mia_vm_plugin_make_none;
	table->make_empty = _mia_vm_plugin_make_empty;
	table->heap_alloc = _mia_vm_plugin_heap_alloc;
	table->heap_adopt = _mia_vm_plugin_heap_adopt;
	table->heap_of = _mia_vm_plugin_heap_of;
	table->heap_free = _mia_vm_plugin_heap_free;
	table->write = _mia_vm_plugin_write;
	table->fail = _mia_vm_plugin_fail;
}

// === loading and running ====================================================

/// stops the program with the runtime error 'message' (which it takes).
mia_noreturn static void _mia_vm_stop(_mia_vm_s* const vm, char* const message) {
	mia_free(vm->error);
	vm->error = message;
	longjmp(*vm->stop, 1);
}

static void _mia_vm_vfail(_mia_vm_s* const vm, const char* const format, va_list args) {
	_mia_vm_stop(vm, mia_string_vformat(format, args));
}

static void _mia_vm_fail(_mia_vm_s* const vm, const char* const format, ...) {
	va_list args; va_start(args, format);
	char* const message = mia_string_vformat(format, args);
	va_end(args);
	_mia_vm_stop(vm, message);
}

/// the decoded operations: the general one, and fast cases.
typedef enum {
	_mia_vm_x_step,                                        // anything: _mia_vm_step on the instruction
	_mia_vm_x_const1, _mia_vm_x_load8, _mia_vm_x_store8, _mia_vm_x_load16, _mia_vm_x_store16,
	_mia_vm_x_add, _mia_vm_x_sub, _mia_vm_x_mul,                   // 64-bit integers (no wrapping needed)
	_mia_vm_x_fadd, _mia_vm_x_fsub, _mia_vm_x_fmul,                // doubles
	_mia_vm_x_eq, _mia_vm_x_ne,                                // integer and bool words
	_mia_vm_x_slt, _mia_vm_x_sle, _mia_vm_x_sgt, _mia_vm_x_sge,        // signed
	_mia_vm_x_ult, _mia_vm_x_ule, _mia_vm_x_ugt, _mia_vm_x_uge,        // unsigned
	_mia_vm_x_flt, _mia_vm_x_fle, _mia_vm_x_fgt, _mia_vm_x_fge,        // floats
	_mia_vm_x_jump, _mia_vm_x_jump_false, _mia_vm_x_jump_true,
	_mia_vm_x_range_test, _mia_vm_x_range_next,
	_mia_vm_x_index_slice8, _mia_vm_x_index_slice1,            // elements of 8 bytes, of u8 or bool
	_mia_vm_x_loc_index_slice, _mia_vm_x_load_loc8, _mia_vm_x_store_loc8,
	_mia_vm_x_dup1, _mia_vm_x_pop,
	_mia_vm_x_call, _mia_vm_x_return,
	_mia_vm_x_load, _mia_vm_x_store,                           // other scalars, converted
	_mia_vm_x_load_bytes, _mia_vm_x_store_bytes, _mia_vm_x_zero, _mia_vm_x_const,
	_mia_vm_x_f32add, _mia_vm_x_f32sub, _mia_vm_x_f32mul,          // computed as doubles, rounded to f32
	_mia_vm_x_field, _mia_vm_x_set_field,                      // scalars in a struct on the stack
	_mia_vm_x_not,
	_mia_vm_x_load_loc, _mia_vm_x_store_loc, _mia_vm_x_index_slice, // other scalars, converted
	// an integer comparison and the jump after it (b: the jump's target; it jumps when the comparison fails)
	_mia_vm_x_eq_jump, _mia_vm_x_ne_jump, _mia_vm_x_slt_jump, _mia_vm_x_sle_jump, _mia_vm_x_sgt_jump,
	_mia_vm_x_sge_jump, _mia_vm_x_ult_jump, _mia_vm_x_ule_jump, _mia_vm_x_ugt_jump, _mia_vm_x_uge_jump,
	// runs of instructions (see _mia_vm_fuse): a word local, then a constant word ('load_const'), a word
	// local ('load_load'), the sum or difference of the two (and that stored to a word local), an element
	// of a slice local at a word local index (8 bytes, 1 byte) or its location
	_mia_vm_x_load_const, _mia_vm_x_load_load, _mia_vm_x_add_const, _mia_vm_x_sub_const, _mia_vm_x_add_const_store,
	_mia_vm_x_sub_const_store, _mia_vm_x_index_local8, _mia_vm_x_index_local1, _mia_vm_x_loc_index_local,
	_mia_vm_x_count,
} _mia_vm_x_e;

struct _mia_vm_exec_s {
	uint8_t                           x          ;
	uint8_t                           plain      ;   // the operation of this instruction alone (x is a fused one, or the same)
	const mia_bytecode_instruction_s* instruction;
	int64_t                           a          ;
	int64_t                           b          ;
	uint64_t                          word       ;   // const1: the constant's word
};

// --- arithmetic --------------------------------------------------------------------------------

static _mia_vm_word_u _mia_vm_real(const double value) {
	return (_mia_vm_word_u) { .f = value, };
}

static _mia_vm_word_u _mia_vm_boolean(const bool value) {
	return (_mia_vm_word_u) { .u = value ? 1 : 0, };
}

/// the integer part of 'value' modulo 2^64 (like python's int(), then wrapped).
static uint64_t _mia_vm_truncate(const double value) {
	const double whole = trunc(value);
	if (fabs(whole) < 9223372036854775808.0) return (uint64_t)(int64_t)whole;
	double low = fmod(whole, 18446744073709551616.0);
	if (low < 0) low += 18446744073709551616.0;
	return (low >= 18446744073709551616.0) ? 0 : (uint64_t)low;
}

static _mia_vm_word_u _mia_vm_arithmetic(_mia_vm_s* const vm, const mia_bytecode_instruction_s* const instruction, const _mia_vm_word_u left, const _mia_vm_word_u right) {
	const mia_code_e code = instruction->type;
	const mia_opcode_e opcode = instruction->opcode;
	if (mia_code_is_float(code)) {
		const double a = left.f, b = right.f;
		switch (opcode) {
			case mia_opcode_ADD: return _mia_vm_real(_mia_vm_round(code, a + b));
			case mia_opcode_SUB: return _mia_vm_real(_mia_vm_round(code, a - b));
			case mia_opcode_MUL: return _mia_vm_real(_mia_vm_round(code, a * b));
			case mia_opcode_DIV:
				if (0.0 == b) return _mia_vm_real((0.0 == a || isnan(a)) ? NAN : copysign(INFINITY, a) * copysign(1.0, b));
				return _mia_vm_real(_mia_vm_round(code, a / b));
			default: break;
		}
		_mia_vm_fail(vm, "`%s` on floats", mia_opcode_name(opcode));
	}
	const bool is_signed = mia_code_is_signed(code);
	const uint64_t ua = left.u, ub = right.u;
	switch (opcode) {
		case mia_opcode_ADD: return _mia_vm_wrap(code, ua + ub);
		case mia_opcode_SUB: return _mia_vm_wrap(code, ua - ub);
		case mia_opcode_MUL: return _mia_vm_wrap(code, ua * ub);
		case mia_opcode_DIV:
		case mia_opcode_MOD: {
			if (0 == ub) _mia_vm_fail(vm, "division by zero");
			if (!is_signed) return _mia_vm_wrap(code, mia_opcode_DIV == opcode ? ua / ub : ua % ub);
			if (INT64_MIN == left.i && -1 == right.i) return _mia_vm_wrap(code, mia_opcode_DIV == opcode ? ua : 0);
			return _mia_vm_wrap(code, (uint64_t)(mia_opcode_DIV == opcode ? left.i / right.i : left.i % right.i));
		}
		case mia_opcode_AND: return _mia_vm_wrap(code, ua & ub);
		case mia_opcode_OR:  return _mia_vm_wrap(code, ua | ub);
		case mia_opcode_XOR: return _mia_vm_wrap(code, ua ^ ub);
		case mia_opcode_SHL:
		case mia_opcode_SHR: {
			if (mia_code_is_signed(instruction->target) && right.i < 0) {
				char text[48];
				_mia_vm_fail(vm, "negative shift count %s", _mia_vm_int_text(text, mia_int128_from_int(right.i)));
			}
			const uint64_t count = ub;
			if (mia_opcode_SHL == opcode) return _mia_vm_wrap(code, count >= 64 ? 0 : ua << count);
			if (is_signed) {
				if (count >= 64) return _mia_vm_wrap(code, left.i < 0 ? UINT64_MAX : 0);
				// an arithmetic shift, written so it does not depend on the c implementation
				return _mia_vm_wrap(code, left.i < 0 ? ~(~ua >> count) : ua >> count);
			}
			return _mia_vm_wrap(code, count >= 64 ? 0 : ua >> count);
		}
		default: break;
	}
	_mia_vm_fail(vm, "`%s` on integers", mia_opcode_name(opcode));
}

/// -1, 0 or 1; 2 when unordered (nan).
static int _mia_vm_compare(const mia_code_e code, const _mia_vm_word_u left, const _mia_vm_word_u right) {
	if (mia_code_is_float(code)) return (left.f < right.f) ? -1 : (left.f > right.f) ? 1 : (left.f == right.f) ? 0 : 2;
	if (mia_code_is_signed(code)) return (left.i < right.i) ? -1 : (left.i > right.i) ? 1 : 0;
	return (left.u < right.u) ? -1 : (left.u > right.u) ? 1 : 0;
}

static _mia_vm_word_u _mia_vm_cast(_mia_vm_s* const vm, const mia_bytecode_instruction_s* const instruction, const _mia_vm_word_u value) {
	const mia_code_e source = instruction->type, target = instruction->target;
	if (mia_code_is_float(target)) {
		if (mia_code_is_float(source)) return _mia_vm_real(_mia_vm_round(target, value.f));
		return _mia_vm_real(_mia_vm_round(target, mia_code_is_signed(source) ? (double)value.i : (double)value.u));
	}
	if (mia_code_is_float(source)) {
		if (!isfinite(value.f)) {
			mia_string_s text = {0};
			mia_format_float_value(&text, value.f, 64);
			char buffer[64];
			(void)snprintf(buffer, sizeof(buffer), "%s", mia_string_cstr(&text));
			mia_string_free(&text);
			char name[16];
			size_t length = 0;
			for (const char* at = mia_code_name(target); *at != '\0' && length + 1 < sizeof(name); ++at) name[length++] = (char)(*at | 0x20);
			name[length] = '\0';
			_mia_vm_fail(vm, "cannot convert %s to `%s`", buffer, name);
		}
		return _mia_vm_wrap(target, _mia_vm_truncate(value.f));
	}
	return _mia_vm_wrap(target, value.u);
}

// --- the stack -------------------------------------------------------------------------------

#define _mia_vm_top(_vm) ((_vm)->stack[(_vm)->sp - 1])

static inline void _mia_vm_push(_mia_vm_s* const vm, const _mia_vm_word_u word) {
	vm->stack[vm->sp++] = word;
}

static inline _mia_vm_word_u _mia_vm_pop(_mia_vm_s* const vm) {
	return vm->stack[--vm->sp];
}

static uint64_t _mia_vm_words(const uint64_t size) {
	return (size + 7) / 8;
}

/// pushes 'size' bytes of memory as words.
static void _mia_vm_push_bytes(_mia_vm_s* const vm, const uint8_t* const memory, const uint64_t size) {
	const size_t words = (size_t)_mia_vm_words(size);
	if (words > 0) {
		vm->stack[vm->sp + words - 1].u = 0;
		memcpy(vm->stack + vm->sp, memory, (size_t)size);
	}
	vm->sp += words;
}

/// whether values of 'code' are moved as bytes (slices, structs, arrays)
/// rather than converted to and from a word.
static bool _mia_vm_as_bytes(const mia_code_e code) {
	return mia_code_slice == code || mia_code_bytes == code;
}

/// pushes the value of type 'code' ('size' bytes) that is in memory.
static void _mia_vm_push_value(_mia_vm_s* const vm, const mia_code_e code, const uint8_t* const memory, const uint64_t size) {
	if (_mia_vm_as_bytes(code)) _mia_vm_push_bytes(vm, memory, size);
	else _mia_vm_push(vm, _mia_vm_read(code, memory));
}

/// pops a value of type 'code' ('size' bytes) into memory.
static void _mia_vm_pop_value(_mia_vm_s* const vm, const mia_code_e code, uint8_t* const memory, const uint64_t size) {
	vm->sp -= (size_t)_mia_vm_value_words(code, size);
	if (_mia_vm_as_bytes(code)) memmove(memory, vm->stack + vm->sp, (size_t)size);
	else _mia_vm_write(code, memory, vm->stack[vm->sp]);
}

/// replaces the 'words' words at 'start' with the value of 'code' ('size' bytes) at 'memory' (inside them).
static void _mia_vm_replace_with(_mia_vm_s* const vm, const size_t start, const mia_code_e code, const uint8_t* const memory, const uint64_t size) {
	if (!_mia_vm_as_bytes(code)) {
		const _mia_vm_word_u word = _mia_vm_read(code, memory);
		vm->sp = start;
		_mia_vm_push(vm, word);
		return;
	}
	const size_t words = (size_t)_mia_vm_words(size);
	memmove(vm->stack + start, memory, (size_t)size);
	if (size % 8 != 0) memset((uint8_t*)(vm->stack + start) + size, 0, (size_t)(words * 8 - size));
	vm->sp = start + words;
}

static _mia_vm_frame_s* _mia_vm_frame(_mia_vm_s* const vm) {
	return &vm->frames.data[vm->frames.count - 1];
}

/// stops: an index out of bounds.
mia_noreturn static void _mia_vm_out_of_bounds(_mia_vm_s* const vm, const mia_code_e code, const _mia_vm_word_u word, const uint64_t length, const bool slice) {
	char text[48];
	_mia_vm_int_text(text, _mia_vm_integer(code, word));
	if (slice) _mia_vm_fail(vm, "index %s is out of bounds (length %lld)", text, (long long)length);
	_mia_vm_fail(vm, "index %s is out of bounds (length %llu)", text, (unsigned long long)length);
}

/// an index of type 'code' in [0, length), or stops. negative indexes are
/// large as unsigned words, so one comparison checks both ends.
static inline uint64_t _mia_vm_index(_mia_vm_s* const vm, const mia_code_e code, const _mia_vm_word_u word, const uint64_t length, const bool slice) {
	if (word.u >= length) _mia_vm_out_of_bounds(vm, code, word, length, slice);
	return word.u;
}

/// pops a location: the memory of 'size' bytes there (alive and in bounds) and its handle.
static uint8_t* _mia_vm_pop_location(_mia_vm_s* const vm, const uint64_t size, uint64_t* const handle) {
	vm->sp -= 2;
	*handle = vm->stack[vm->sp].u;
	return _mia_vm_memory(vm, *handle, vm->stack[vm->sp + 1].u, size);
}

/// the bounds [low, high) of a slice of something of 'length' elements: pops them.
static void _mia_vm_slice_bounds(_mia_vm_s* const vm, const mia_bytecode_instruction_s* const instruction, const uint64_t length, uint64_t* const low_out,
								 uint64_t* const high_out) {
	const int64_t flags = instruction->a;
	const bool has_high = (flags & mia_bytecode_slice_high) != 0, has_low = (flags & mia_bytecode_slice_low) != 0;
	const mia_int128_s high_value = has_high ? _mia_vm_integer(instruction->target, _mia_vm_pop(vm)) : mia_int128_zero();
	const mia_int128_s low = has_low ? _mia_vm_integer(instruction->type, _mia_vm_pop(vm)) : mia_int128_zero();
	const mia_int128_s high = has_high ? mia_int128_add(high_value, mia_int128_from_uint((flags & mia_bytecode_slice_inclusive) ? 1 : 0)) : mia_int128_from_uint(length);
	if (mia_int128_negative(low) || mia_int128_less(high, low) || mia_int128_less(mia_int128_from_uint(length), high)) {
		char low_text[48], high_text[48];
		_mia_vm_fail(vm, "slice %s..%s is out of bounds (length %lld)", _mia_vm_int_text(low_text, low), _mia_vm_int_text(high_text, high), (long long)length);
	}
	*low_out = mia_int128_low(low);
	*high_out = mia_int128_low(high);
}

/// pushes a slice; its offset and length must fit the slice format.
static void _mia_vm_push_slice(_mia_vm_s* const vm, const uint64_t handle, const uint64_t offset, const uint64_t length) {
	if (offset > 0xFFFFFFFFu || length > 0xFFFFFFFFu) _mia_vm_fail(vm, "a slice of more than 4 GiB");
	vm->stack[vm->sp++].u = handle;
	vm->stack[vm->sp++].u = _mia_vm_slice_word(offset, length);
}

// --- calls -----------------------------------------------------------------------------------

/// clears the byte area of a frame (a multiple of 16 bytes): a small one with
/// fixed stores, which cost less than a call of memset, a bigger one with it.
static void _mia_vm_clear(uint8_t* const base, const size_t size) {
	static const uint8_t zero[16] = {0};
	switch (size) {
		case 64: memcpy(base + 48, zero, 16); // fallthrough
		case 48: memcpy(base + 32, zero, 16); // fallthrough
		case 32: memcpy(base + 16, zero, 16); // fallthrough
		case 16: memcpy(base, zero, 16); // fallthrough
		case 0:  return;
		default: memset(base, 0, size);
	}
}

static void _mia_vm_call(_mia_vm_s* const vm, const int64_t index) {
	if (vm->frames.count >= vm->call_depth) _mia_vm_fail(vm, "stack overflow: more than %llu nested calls", (unsigned long long)vm->call_depth);
	const mia_bytecode_function_s* const function = &vm->module->functions.data[index];
	const _mia_vm_prepared_s* const prepared = &vm->prepared.data[index];
	if (vm->frames.count == vm->frames.capacity) mia_vector_reserve(&vm->frames, vm->frames.capacity * 2 + 16);
	_mia_vm_frame_s* const frame = &vm->frames.data[vm->frames.count];
	*frame = (_mia_vm_frame_s) { .function = index, .code = prepared->code, };
	// the byte area: from the chunk in use when it fits
	_mia_vm_chunk_s* const chunk = (vm->chunk < vm->chunks.count) ? &vm->chunks.data[vm->chunk] : NULL;
	const size_t rounded = prepared->frame;
	// the locals of all running calls together get as much as one function may have
	if (rounded > mia_bytecode_frame_size_limit - vm->frame_bytes) {
		_mia_vm_fail(vm, "stack overflow: the locals of the running calls would take more than %llu bytes",
			(unsigned long long)mia_bytecode_frame_size_limit);
	}
	vm->frame_bytes += rounded;
	if (chunk != NULL && chunk->size - chunk->used >= rounded) {
		frame->chunk = vm->chunk;
		frame->used = chunk->used;
		frame->base = chunk->data + chunk->used;
		chunk->used += rounded;
		// (measured: fixed stores suit frames whose arguments are words, memset those that get
		// struct arguments copied in; either way the whole area is cleared)
		if (prepared->packed) _mia_vm_clear(frame->base, rounded);
		else memset(frame->base, 0, rounded);
	}
	else {
		frame->base = _mia_vm_frame_alloc(vm, frame, function->frame);
	}
	// the arguments, first to last, from the operand stack into the frame
	const _mia_vm_word_u* argument = vm->stack + vm->sp - prepared->arguments;
	if (prepared->packed) {
		for (uint64_t i = 0; i < prepared->arguments; ++i) memcpy(frame->base + 8 * i, &argument[i], 8);
	}
	else {
		for (size_t i = 0; i < function->slots.count; ++i) {
			const mia_bytecode_slot_s* const slot = &function->slots.data[i];
			uint8_t* const memory = frame->base + slot->offset;
			if (_mia_vm_as_bytes(slot->code)) memcpy(memory, argument, (size_t)slot->size);
			else _mia_vm_write(slot->code, memory, *argument);
			argument += _mia_vm_value_words(slot->code, slot->size);
		}
	}
	vm->sp -= (size_t)prepared->arguments;
	frame->stack_base = vm->sp;
	vm->frames.count += 1;
	if (vm->capacity - vm->sp < prepared->max_stack + 2) _mia_vm_reserve(vm, prepared->max_stack + 2);
}

static void _mia_vm_return(_mia_vm_s* const vm, const int64_t words) {
	_mia_vm_frame_s* const frame = _mia_vm_frame(vm);
	const size_t base = frame->stack_base;
	if (1 == vm->frames.count && words > 0) {
		vm->returned = true;
		vm->result = _mia_vm_top(vm);
	}
	// most functions give one word or none
	if (1 == words) vm->stack[base] = vm->stack[vm->sp - 1];
	else if (words > 1) memmove(vm->stack + base, vm->stack + vm->sp - words, (size_t)words * sizeof(_mia_vm_word_u));
	vm->sp = base + (size_t)words;
	_mia_vm_frame_release(vm, frame);
	vm->frames.count -= 1;
}

/// a call of a function value: the value sits below the arguments.
static void _mia_vm_call_value(_mia_vm_s* const vm, const int64_t words, const int64_t result) {
	const size_t slot = vm->sp - 1 - (size_t)words;
	const _mia_vm_word_u function = vm->stack[slot];
	memmove(vm->stack + slot, vm->stack + slot + 1, (size_t)words * sizeof(_mia_vm_word_u));
	vm->sp -= 1;
	const uint64_t index = function.u & ~((uint64_t)1 << 63);
	if (function.u >> 63) {
		if (index >= vm->links.count) _mia_vm_fail(vm, "a call of a function value that is not one");
		const mia_bytecode_intrinsic_s* const intrinsic = &vm->module->intrinsics.data[index];
		if (_mia_vm_intrinsic_arguments(vm, intrinsic) != (uint64_t)words || _mia_vm_intrinsic_result(vm, intrinsic) != (uint64_t)result) {
			_mia_vm_fail(vm, "a call of a function value of another type (the module is broken)");
		}
		_mia_vm_call_plugin(vm, (int64_t)index, words);
		return;
	}
	if (index >= vm->module->functions.count) _mia_vm_fail(vm, "a call of a function value that is not one");
	if (vm->prepared.data[index].arguments != (uint64_t)words || vm->module->functions.data[index].result != (uint64_t)result) {
		_mia_vm_fail(vm, "a call of a function value of another type (the module is broken)");
	}
	_mia_vm_call(vm, (int64_t)index);
}

// --- the general step --------------------------------------------------------------------------

static void _mia_vm_step(_mia_vm_s* const vm, _mia_vm_frame_s* const frame, const mia_bytecode_instruction_s* const instruction) {
	uint8_t* const base = frame->base;
	const int64_t a = instruction->a;
	const mia_code_e code = instruction->type;
	switch (instruction->opcode) {
		case mia_opcode_CONST: {
			const _mia_vm_constant_s* const constant = &vm->constants.data[a];
			if (constant->count > 0) memcpy(vm->stack + vm->sp, constant->words, constant->count * sizeof(_mia_vm_word_u));
			vm->sp += constant->count;
			break;
		}
		case mia_opcode_ZERO:
			memset(vm->stack + vm->sp, 0, (size_t)a * sizeof(_mia_vm_word_u));
			vm->sp += (size_t)a;
			break;
		case mia_opcode_POP: vm->sp -= (size_t)a; break;
		case mia_opcode_DUP:
			memcpy(vm->stack + vm->sp, vm->stack + vm->sp - a, (size_t)a * sizeof(_mia_vm_word_u));
			vm->sp += (size_t)a;
			break;
		case mia_opcode_LOAD:  _mia_vm_push_value(vm, code, base + a, (uint64_t)instruction->b); break;
		case mia_opcode_STORE: _mia_vm_pop_value(vm, code, base + a, (uint64_t)instruction->b); break;
		case mia_opcode_LOC_LOCAL:
			vm->stack[vm->sp++].u = _mia_vm_frame_handle(vm, frame);
			vm->stack[vm->sp++].u = (uint64_t)a;
			break;
		case mia_opcode_LOC_CONST:
			vm->stack[vm->sp++].u = _mia_vm_constant_handle(vm, a);
			vm->stack[vm->sp++].u = 0;
			break;
		case mia_opcode_LOC_FIELD: _mia_vm_top(vm).u += (uint64_t)a; break;
		case mia_opcode_LOC_INDEX: {
			const uint64_t index = _mia_vm_index(vm, code, _mia_vm_pop(vm), (uint64_t)instruction->b, false);
			_mia_vm_top(vm).u += index * (uint64_t)a;
			break;
		}
		case mia_opcode_LOC_INDEX_SLICE: {
			const _mia_vm_word_u index_word = _mia_vm_pop(vm);
			const uint64_t slice = _mia_vm_top(vm).u;
			const uint64_t handle = vm->stack[vm->sp - 2].u;
			const uint64_t index = _mia_vm_index(vm, code, index_word, _mia_vm_slice_length(slice), true);
			const uint64_t offset = _mia_vm_slice_offset(slice) + index * (uint64_t)a;
			(void)_mia_vm_memory(vm, handle, offset, (uint64_t)a);
			_mia_vm_top(vm).u = offset;
			break;
		}
		case mia_opcode_TEMP_LOC:
			_mia_vm_pop_value(vm, code, base + a, (uint64_t)instruction->b);
			vm->stack[vm->sp++].u = _mia_vm_frame_handle(vm, frame);
			vm->stack[vm->sp++].u = (uint64_t)a;
			break;
		case mia_opcode_LOAD_LOC: {
			uint64_t handle = 0;
			_mia_vm_push_value(vm, code, _mia_vm_pop_location(vm, (uint64_t)a, &handle), (uint64_t)a);
			break;
		}
		case mia_opcode_STORE_LOC: {
			const size_t value = vm->sp - (size_t)_mia_vm_value_words(code, (uint64_t)a);
			vm->sp = value;
			uint64_t handle = 0;
			uint8_t* const memory = _mia_vm_pop_location(vm, (uint64_t)a, &handle);
			if (handle & _mia_vm_handle_read_only) _mia_vm_fail(vm, "write through a read-only slice");
			if (_mia_vm_as_bytes(code)) memmove(memory, vm->stack + value, (size_t)a);
			else _mia_vm_write(code, memory, vm->stack[value]);
			break;
		}
		case mia_opcode_FIELD: {
			const size_t start = vm->sp - (size_t)instruction->b;
			_mia_vm_replace_with(vm, start, code, (const uint8_t*)(vm->stack + start) + a, (uint64_t)instruction->c);
			break;
		}
		case mia_opcode_SET_FIELD: {
			const size_t value = vm->sp - (size_t)_mia_vm_value_words(code, (uint64_t)instruction->c);
			_mia_vm_pop_value(vm, code, (uint8_t*)(vm->stack + value - instruction->b) + a, (uint64_t)instruction->c);
			break;
		}
		case mia_opcode_MAKE_ARRAY: {
			const uint64_t size = (uint64_t)instruction->b;
			const size_t element_words = (size_t)_mia_vm_value_words(code, size);
			const size_t start = vm->sp - (size_t)a * element_words;
			mia_string_s* const scratch = &vm->bytes;
			mia_string_clear(scratch);
			mia_string_append_repeat(scratch, '\0', (size_t)(size * (uint64_t)a) + 8);
			uint8_t* const packed = (uint8_t*)scratch->data;
			for (int64_t i = 0; i < a; ++i) {
				const _mia_vm_word_u* const item = vm->stack + start + (size_t)i * element_words;
				uint8_t* const memory = packed + (uint64_t)i * size;
				if (_mia_vm_as_bytes(code)) memcpy(memory, item, (size_t)size);
				else _mia_vm_write(code, memory, *item);
			}
			vm->sp = start;
			_mia_vm_push_bytes(vm, packed, size * (uint64_t)a);
			break;
		}
		case mia_opcode_INDEX: {
			const uint64_t index = _mia_vm_index(vm, instruction->target, _mia_vm_pop(vm), (uint64_t)a, false);
			const size_t start = vm->sp - (size_t)instruction->b;
			const uint64_t size = (uint64_t)instruction->c;
			_mia_vm_replace_with(vm, start, code, (const uint8_t*)(vm->stack + start) + index * size, size);
			break;
		}
		case mia_opcode_INDEX_SLICE: {
			const _mia_vm_word_u index_word = _mia_vm_pop(vm);
			vm->sp -= 2;
			const uint64_t handle = vm->stack[vm->sp].u, slice = vm->stack[vm->sp + 1].u;
			const uint64_t index = _mia_vm_index(vm, instruction->target, index_word, _mia_vm_slice_length(slice), true);
			const uint64_t size = (uint64_t)a;
			_mia_vm_push_value(vm, code, _mia_vm_memory(vm, handle, _mia_vm_slice_offset(slice) + index * size, size), size);
			break;
		}
		case mia_opcode_LEN_SLICE: {
			const uint64_t slice = _mia_vm_pop(vm).u;
			_mia_vm_top(vm).u = _mia_vm_slice_length(slice);
			break;
		}
		case mia_opcode_SLICE_IS_NULL: {
			const uint64_t slice = _mia_vm_pop(vm).u;
			_mia_vm_top(vm).u = 0 == _mia_vm_slice_length(slice);
			break;
		}
		case mia_opcode_SLICE_LOC: {
			uint64_t low = 0, high = 0;
			_mia_vm_slice_bounds(vm, instruction, (uint64_t)instruction->b, &low, &high);
			vm->sp -= 2;
			uint64_t handle = vm->stack[vm->sp].u;
			const uint64_t offset = vm->stack[vm->sp + 1].u;
			if (a & mia_bytecode_slice_read_only) handle |= _mia_vm_handle_read_only;
			_mia_vm_push_slice(vm, handle, offset + low * (uint64_t)instruction->c, high - low);
			break;
		}
		case mia_opcode_SLICE_SLICE: {
			const size_t below = vm->sp - (((a & mia_bytecode_slice_low) != 0) ? 1 : 0) - (((a & mia_bytecode_slice_high) != 0) ? 1 : 0);
			const uint64_t handle = vm->stack[below - 2].u, slice = vm->stack[below - 1].u;
			uint64_t low = 0, high = 0;
			_mia_vm_slice_bounds(vm, instruction, _mia_vm_slice_length(slice), &low, &high);
			vm->sp -= 2;
			if (0 == (handle & ~_mia_vm_handle_read_only)) {
				_mia_vm_push_slice(vm, 0, 0, 0);
				break;
			}
			(void)_mia_vm_resolve(vm, handle);
			_mia_vm_push_slice(vm, handle, _mia_vm_slice_offset(slice) + low * (uint64_t)instruction->b, high - low);
			break;
		}
		case mia_opcode_JUMP: frame->pc = a; break;
		case mia_opcode_JUMP_IF_FALSE: if (0 == _mia_vm_pop(vm).u) frame->pc = a; break;
		case mia_opcode_JUMP_IF_TRUE:  if (0 != _mia_vm_pop(vm).u) frame->pc = a; break;
		case mia_opcode_NOT: _mia_vm_top(vm).u = 0 == _mia_vm_top(vm).u; break;
		case mia_opcode_RANGE_INIT: {
			const _mia_vm_word_u end = _mia_vm_pop(vm), start = _mia_vm_pop(vm);
			const bool up = mia_code_is_signed(code) ? end.i >= start.i : end.u >= start.u;
			const uint64_t step = up ? 1 : UINT64_MAX;
			const uint64_t state[3] = { start.u, step, instruction->b ? end.u + step : end.u, };
			memcpy(base + a, state, sizeof(state));
			break;
		}
		case mia_opcode_RANGE_TEST: {
			uint64_t state[3];
			memcpy(state, base + a, sizeof(state));
			if (state[0] == state[2]) frame->pc = instruction->b;
			break;
		}
		case mia_opcode_RANGE_STEP: {
			uint64_t state[2];
			memcpy(state, base + a, sizeof(state));
			state[0] += state[1];
			memcpy(base + a, state, sizeof(state[0]));
			break;
		}
		case mia_opcode_RANGE_NEXT: {
			uint64_t state[3];
			memcpy(state, base + a, sizeof(state));
			state[0] += state[1];
			memcpy(base + a, state, sizeof(state[0]));
			if (state[0] != state[2]) frame->pc = instruction->b;
			break;
		}
		case mia_opcode_CALL:           _mia_vm_call(vm, a); break;
		case mia_opcode_CALL_VALUE:     _mia_vm_call_value(vm, a, instruction->b); break;
		case mia_opcode_CALL_INTRINSIC: _mia_vm_call_plugin(vm, a, instruction->b); break;
		case mia_opcode_FUNCTION:       _mia_vm_push(vm, (_mia_vm_word_u) { .u = (uint64_t)a, }); break;
		case mia_opcode_INTRINSIC:      _mia_vm_push(vm, (_mia_vm_word_u) { .u = (uint64_t)a | ((uint64_t)1 << 63), }); break;
		case mia_opcode_RETURN:         _mia_vm_return(vm, a); break;
		case mia_opcode_ASSERT_FAIL: {
			const mia_bytecode_constant_s* const message = &vm->module->constants.data[a];
			_mia_vm_fail(vm, "%s", mia_bytecode_constant_text == message->kind ? message->text : "assertion failed");
		}
		case mia_opcode_EQ: case mia_opcode_NE: case mia_opcode_LT: case mia_opcode_GT: case mia_opcode_LE: case mia_opcode_GE: {
			const _mia_vm_word_u right = _mia_vm_pop(vm);
			const int order = _mia_vm_compare(code, _mia_vm_top(vm), right);
			bool result = false;
			switch (instruction->opcode) {
				case mia_opcode_EQ: result = 0 == order; break;
				case mia_opcode_NE: result = 0 != order; break;
				case mia_opcode_LT: result = -1 == order; break;
				case mia_opcode_GT: result = 1 == order; break;
				case mia_opcode_LE: result = -1 == order || 0 == order; break;
				default:       result = 1 == order || 0 == order; break;
			}
			_mia_vm_top(vm) = _mia_vm_boolean(result);
			break;
		}
		case mia_opcode_ADD: case mia_opcode_SUB: case mia_opcode_MUL: case mia_opcode_DIV: case mia_opcode_MOD:
		case mia_opcode_AND: case mia_opcode_OR: case mia_opcode_XOR: case mia_opcode_SHL: case mia_opcode_SHR: {
			const _mia_vm_word_u right = _mia_vm_pop(vm);
			_mia_vm_top(vm) = _mia_vm_arithmetic(vm, instruction, _mia_vm_top(vm), right);
			break;
		}
		case mia_opcode_NEG:
			if (mia_code_is_float(code)) _mia_vm_top(vm) = _mia_vm_real(_mia_vm_round(code, -_mia_vm_top(vm).f));
			else _mia_vm_top(vm) = _mia_vm_wrap(code, (uint64_t)0 - _mia_vm_top(vm).u);
			break;
		case mia_opcode_BITNOT: _mia_vm_top(vm) = _mia_vm_wrap(code, ~_mia_vm_top(vm).u); break;
		case mia_opcode_CAST: _mia_vm_top(vm) = _mia_vm_cast(vm, instruction, _mia_vm_top(vm)); break;
		case mia_opcode_count: _mia_vm_fail(vm, "an invalid instruction");
	}
}

// --- the loop --------------------------------------------------------------------------------

/// the loop of a profiled run: every instruction through the general step,
/// counted (with the instruction that ran before it, when it is the one
/// before it in the code). it runs what the fast loop runs, instruction for
/// instruction, so the counts and the limit are the same.
static void _mia_vm_profile_loop(_mia_vm_s* const vm) {
	const uint64_t stop_at = (vm->options->limit > 0) ? vm->options->limit : UINT64_MAX;
	mia_vm_profile_function_s* const functions = vm->options->profile->functions.data;
	size_t last_depth = 0;
	int64_t last_function = -1, last_pc = -1;
	functions[_mia_vm_frame(vm)->function].calls += 1;
	while (vm->frames.count > 0) {
		if (vm->steps >= stop_at) _mia_vm_fail(vm, "stopped after %llu instructions", (unsigned long long)stop_at);
		_mia_vm_frame_s* const frame = _mia_vm_frame(vm);
		const int64_t pc = frame->pc;
		mia_vm_profile_function_s* const counts = &functions[frame->function];
		if (vm->frames.count == last_depth && frame->function == last_function && pc == last_pc + 1) counts->followed[last_pc] += 1;
		counts->runs[pc] += 1;
		last_depth = vm->frames.count;
		last_function = frame->function;
		last_pc = pc;
		frame->pc = pc + 1;
		vm->steps += 1;
		_mia_vm_step(vm, frame, frame->code[pc].instruction);
		if (vm->frames.count > last_depth) functions[_mia_vm_frame(vm)->function].calls += 1;
	}
}

/// whether the loop dispatches with computed goto (gcc and clang; the
/// portable build and msvc use a switch).
#if defined(__GNUC__) && !defined(mia_portable)
#	define _mia_vm_threaded 1
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wpedantic"
#else
#	define _mia_vm_threaded 0
#endif

static void _mia_vm_loop(_mia_vm_s* const vm) {
	const uint64_t stop_at = (vm->options->limit > 0) ? vm->options->limit : UINT64_MAX;
	uint64_t steps = vm->steps;
	_mia_vm_frame_s* frame = _mia_vm_frame(vm);
	const _mia_vm_exec_s* code = frame->code;
	int64_t pc = frame->pc;
	uint8_t* base = frame->base;
	_mia_vm_word_u* sp = vm->stack + vm->sp;
	// the state lives in locals; these move it to and from the machine around general steps
#define _mia_vm_save() do { vm->sp = (size_t)(sp - vm->stack); frame->pc = pc; vm->steps = steps; } while (0)
#define _mia_vm_load() do {                                                    \
		if (0 == vm->frames.count) return;                                     \
		frame = _mia_vm_frame(vm);                                             \
		code = frame->code;                                                    \
		pc = frame->pc;                                                        \
		base = frame->base;                                                    \
		sp = vm->stack + vm->sp;                                               \
	} while (0)
	const _mia_vm_exec_s* exec = NULL;
#if _mia_vm_threaded
	// every operation ends in its own jump to the next one
	static const void* const labels[_mia_vm_x_count] = {
		[_mia_vm_x_step] = &&op_step, [_mia_vm_x_const1] = &&op_const1, [_mia_vm_x_load8] = &&op_load8,
		[_mia_vm_x_store8] = &&op_store8, [_mia_vm_x_load16] = &&op_load16,
		[_mia_vm_x_store16] = &&op_store16, [_mia_vm_x_add] = &&op_add, [_mia_vm_x_sub] = &&op_sub,
		[_mia_vm_x_mul] = &&op_mul, [_mia_vm_x_fadd] = &&op_fadd, [_mia_vm_x_fsub] = &&op_fsub,
		[_mia_vm_x_fmul] = &&op_fmul, [_mia_vm_x_eq] = &&op_eq, [_mia_vm_x_ne] = &&op_ne,
		[_mia_vm_x_slt] = &&op_slt, [_mia_vm_x_sle] = &&op_sle, [_mia_vm_x_sgt] = &&op_sgt,
		[_mia_vm_x_sge] = &&op_sge, [_mia_vm_x_ult] = &&op_ult, [_mia_vm_x_ule] = &&op_ule,
		[_mia_vm_x_ugt] = &&op_ugt, [_mia_vm_x_uge] = &&op_uge, [_mia_vm_x_flt] = &&op_flt,
		[_mia_vm_x_fle] = &&op_fle, [_mia_vm_x_fgt] = &&op_fgt, [_mia_vm_x_fge] = &&op_fge,
		[_mia_vm_x_jump] = &&op_jump, [_mia_vm_x_jump_false] = &&op_jump_false,
		[_mia_vm_x_jump_true] = &&op_jump_true, [_mia_vm_x_range_test] = &&op_range_test,
		[_mia_vm_x_range_next] = &&op_range_next, [_mia_vm_x_index_slice8] = &&op_index_slice8,
		[_mia_vm_x_index_slice1] = &&op_index_slice1, [_mia_vm_x_loc_index_slice] = &&op_loc_index_slice,
		[_mia_vm_x_load_loc8] = &&op_load_loc8, [_mia_vm_x_store_loc8] = &&op_store_loc8,
		[_mia_vm_x_dup1] = &&op_dup1, [_mia_vm_x_pop] = &&op_pop, [_mia_vm_x_call] = &&op_call,
		[_mia_vm_x_return] = &&op_return, [_mia_vm_x_load] = &&op_load, [_mia_vm_x_store] = &&op_store,
		[_mia_vm_x_load_bytes] = &&op_load_bytes, [_mia_vm_x_store_bytes] = &&op_store_bytes,
		[_mia_vm_x_zero] = &&op_zero, [_mia_vm_x_const] = &&op_const, [_mia_vm_x_f32add] = &&op_f32add,
		[_mia_vm_x_f32sub] = &&op_f32sub, [_mia_vm_x_f32mul] = &&op_f32mul, [_mia_vm_x_field] = &&op_field,
		[_mia_vm_x_set_field] = &&op_set_field, [_mia_vm_x_not] = &&op_not,
		[_mia_vm_x_load_loc] = &&op_load_loc, [_mia_vm_x_store_loc] = &&op_store_loc,
		[_mia_vm_x_index_slice] = &&op_index_slice, [_mia_vm_x_eq_jump] = &&op_eq_jump, [_mia_vm_x_ne_jump] = &&op_ne_jump,
		[_mia_vm_x_slt_jump] = &&op_slt_jump, [_mia_vm_x_sle_jump] = &&op_sle_jump, [_mia_vm_x_sgt_jump] = &&op_sgt_jump,
		[_mia_vm_x_sge_jump] = &&op_sge_jump, [_mia_vm_x_ult_jump] = &&op_ult_jump, [_mia_vm_x_ule_jump] = &&op_ule_jump,
		[_mia_vm_x_ugt_jump] = &&op_ugt_jump, [_mia_vm_x_uge_jump] = &&op_uge_jump,
		[_mia_vm_x_load_const] = &&op_load_const, [_mia_vm_x_load_load] = &&op_load_load, [_mia_vm_x_add_const] = &&op_add_const,
		[_mia_vm_x_sub_const] = &&op_sub_const, [_mia_vm_x_add_const_store] = &&op_add_const_store,
		[_mia_vm_x_sub_const_store] = &&op_sub_const_store, [_mia_vm_x_index_local8] = &&op_index_local8,
		[_mia_vm_x_index_local1] = &&op_index_local1, [_mia_vm_x_loc_index_local] = &&op_loc_index_local,
	};
#	define _mia_vm_op(_name) op_##_name:
#	define _mia_vm_next()                                                      \
		do {                                                                   \
			if (steps >= stop_at) goto stopped;                                \
			exec = &code[pc++];                                                \
			steps += 1;                                                        \
			goto *labels[exec->x];                                             \
		} while (0)
#	define _mia_vm_plain() goto *labels[exec->plain]
	// every operation has its entry (one added to _mia_vm_x_e needs one here too)
	for (size_t i = 0; i < _mia_vm_x_count; ++i) mia_assert(labels[i] != NULL);
	_mia_vm_next();
	{
#else
#	define _mia_vm_op(_name) case _mia_vm_x_##_name:
#	define _mia_vm_next() break
#	define _mia_vm_plain() do { operation = exec->plain; goto again; } while (0)
	uint8_t operation = 0;
	for (;;) {
		if (steps >= stop_at) {
			_mia_vm_save();
			_mia_vm_fail(vm, "stopped after %llu instructions", (unsigned long long)stop_at);
		}
		exec = &code[pc++];
		steps += 1;
		operation = exec->x;
	again:
		switch ((_mia_vm_x_e)operation) {
#endif
			_mia_vm_op(const1)   (sp++)->u = exec->word; _mia_vm_next();
			_mia_vm_op(load8)    memcpy(sp++, base + exec->a, 8); _mia_vm_next();
			_mia_vm_op(store8)   memcpy(base + exec->a, --sp, 8); _mia_vm_next();
			_mia_vm_op(load16)   memcpy(sp, base + exec->a, 16); sp += 2; _mia_vm_next();
			_mia_vm_op(store16)  sp -= 2; memcpy(base + exec->a, sp, 16); _mia_vm_next();
			_mia_vm_op(dup1)     *sp = sp[-1]; ++sp; _mia_vm_next();
			_mia_vm_op(pop)      sp -= exec->a; _mia_vm_next();
			_mia_vm_op(add)      --sp; sp[-1].u += sp->u; _mia_vm_next();
			_mia_vm_op(sub)      --sp; sp[-1].u -= sp->u; _mia_vm_next();
			_mia_vm_op(mul)      --sp; sp[-1].u *= sp->u; _mia_vm_next();
			_mia_vm_op(fadd)     --sp; sp[-1].f += sp->f; _mia_vm_next();
			_mia_vm_op(fsub)     --sp; sp[-1].f -= sp->f; _mia_vm_next();
			_mia_vm_op(fmul)     --sp; sp[-1].f *= sp->f; _mia_vm_next();
			_mia_vm_op(eq)       --sp; sp[-1].u = sp[-1].u == sp->u; _mia_vm_next();
			_mia_vm_op(ne)       --sp; sp[-1].u = sp[-1].u != sp->u; _mia_vm_next();
			_mia_vm_op(slt)      --sp; sp[-1].u = sp[-1].i < sp->i; _mia_vm_next();
			_mia_vm_op(sle)      --sp; sp[-1].u = sp[-1].i <= sp->i; _mia_vm_next();
			_mia_vm_op(sgt)      --sp; sp[-1].u = sp[-1].i > sp->i; _mia_vm_next();
			_mia_vm_op(sge)      --sp; sp[-1].u = sp[-1].i >= sp->i; _mia_vm_next();
			_mia_vm_op(ult)      --sp; sp[-1].u = sp[-1].u < sp->u; _mia_vm_next();
			_mia_vm_op(ule)      --sp; sp[-1].u = sp[-1].u <= sp->u; _mia_vm_next();
			_mia_vm_op(ugt)      --sp; sp[-1].u = sp[-1].u > sp->u; _mia_vm_next();
			_mia_vm_op(uge)      --sp; sp[-1].u = sp[-1].u >= sp->u; _mia_vm_next();
			_mia_vm_op(flt)      --sp; sp[-1].u = sp[-1].f < sp->f; _mia_vm_next();
			_mia_vm_op(fle)      --sp; sp[-1].u = sp[-1].f <= sp->f; _mia_vm_next();
			_mia_vm_op(fgt)      --sp; sp[-1].u = sp[-1].f > sp->f; _mia_vm_next();
			_mia_vm_op(fge)      --sp; sp[-1].u = sp[-1].f >= sp->f; _mia_vm_next();
			_mia_vm_op(jump)       pc = exec->a; _mia_vm_next();
			_mia_vm_op(jump_false) if (0 == (--sp)->u) pc = exec->a; _mia_vm_next();
			_mia_vm_op(jump_true)  if (0 != (--sp)->u) pc = exec->a; _mia_vm_next();
			_mia_vm_op(range_test) {
				uint64_t current, stop;
				memcpy(&current, base + exec->a, 8);
				memcpy(&stop, base + exec->a + 16, 8);
				if (current == stop) pc = exec->b;
				_mia_vm_next();
			}
			_mia_vm_op(range_next) {
				uint64_t state[3];
				memcpy(state, base + exec->a, sizeof(state));
				state[0] += state[1];
				memcpy(base + exec->a, state, 8);
				if (state[0] != state[2]) pc = exec->b;
				_mia_vm_next();
			}
			_mia_vm_op(index_slice8)
			_mia_vm_op(index_slice1) {
				const _mia_vm_word_u index = sp[-1];
				const uint64_t handle = sp[-3].u, slice = sp[-2].u;
				if (index.u >= _mia_vm_slice_length(slice)) {
					_mia_vm_save();
					_mia_vm_out_of_bounds(vm, exec->instruction->target, index, _mia_vm_slice_length(slice), true);
				}
				sp -= 3;
				if (_mia_vm_x_index_slice8 == exec->x) {
					_mia_vm_save();
					memcpy(sp, _mia_vm_memory(vm, handle, _mia_vm_slice_offset(slice) + index.u * 8, 8), 8);
				}
				else {
					_mia_vm_save();
					sp->u = *_mia_vm_memory(vm, handle, _mia_vm_slice_offset(slice) + index.u, 1);
					if (mia_code_bool == exec->instruction->type) sp->u = sp->u != 0;
				}
				sp += 1;
				_mia_vm_next();
			}
			_mia_vm_op(loc_index_slice) {
				const _mia_vm_word_u index = sp[-1];
				const uint64_t handle = sp[-3].u, slice = sp[-2].u;
				if (index.u >= _mia_vm_slice_length(slice)) {
					_mia_vm_save();
					_mia_vm_out_of_bounds(vm, exec->instruction->type, index, _mia_vm_slice_length(slice), true);
				}
				const uint64_t offset = _mia_vm_slice_offset(slice) + index.u * (uint64_t)exec->a;
				_mia_vm_save();
				(void)_mia_vm_memory(vm, handle, offset, (uint64_t)exec->a);
				sp -= 1;
				sp[-1].u = offset;
				_mia_vm_next();
			}
			_mia_vm_op(load_loc8) {
				_mia_vm_save();
				memcpy(sp - 2, _mia_vm_memory(vm, sp[-2].u, sp[-1].u, 8), 8);
				sp -= 1;
				_mia_vm_next();
			}
			_mia_vm_op(store_loc8) {
				const uint64_t handle = sp[-3].u;
				_mia_vm_save();
				uint8_t* const memory = _mia_vm_memory(vm, handle, sp[-2].u, 8);
				if (handle & _mia_vm_handle_read_only) _mia_vm_fail(vm, "write through a read-only slice");
				memcpy(memory, sp - 1, 8);
				sp -= 3;
				_mia_vm_next();
			}
			_mia_vm_op(load)  *sp++ = _mia_vm_read(exec->instruction->type, base + exec->a); _mia_vm_next();
			_mia_vm_op(store) _mia_vm_write(exec->instruction->type, base + exec->a, *--sp); _mia_vm_next();
			_mia_vm_op(load_bytes) {
				const size_t words = (size_t)((exec->b + 7) / 8);
				sp[words - 1].u = 0;
				memcpy(sp, base + exec->a, (size_t)exec->b);
				sp += words;
				_mia_vm_next();
			}
			_mia_vm_op(store_bytes)
				sp -= (exec->b + 7) / 8;
				memcpy(base + exec->a, sp, (size_t)exec->b);
				_mia_vm_next();
			_mia_vm_op(zero)
				memset(sp, 0, (size_t)exec->a * sizeof(_mia_vm_word_u));
				sp += exec->a;
				_mia_vm_next();
			_mia_vm_op(const) {
				const _mia_vm_constant_s* const constant = &vm->constants.data[exec->a];
				if (constant->count > 0) memcpy(sp, constant->words, constant->count * sizeof(_mia_vm_word_u));
				sp += constant->count;
				_mia_vm_next();
			}
			_mia_vm_op(f32add) --sp; sp[-1].f = (double)(float)(sp[-1].f + sp->f); _mia_vm_next();
			_mia_vm_op(f32sub) --sp; sp[-1].f = (double)(float)(sp[-1].f - sp->f); _mia_vm_next();
			_mia_vm_op(f32mul) --sp; sp[-1].f = (double)(float)(sp[-1].f * sp->f); _mia_vm_next();
			_mia_vm_op(field) {
				_mia_vm_word_u* const start = sp - exec->b;
				*start = _mia_vm_read(exec->instruction->type, (const uint8_t*)start + exec->a);
				sp = start + 1;
				_mia_vm_next();
			}
			_mia_vm_op(set_field) {
				--sp;
				_mia_vm_write(exec->instruction->type, (uint8_t*)(sp - exec->b) + exec->a, *sp);
				_mia_vm_next();
			}
			_mia_vm_op(not) sp[-1].u = 0 == sp[-1].u; _mia_vm_next();
			_mia_vm_op(load_loc) {
				const mia_code_e type = exec->instruction->type;
				_mia_vm_save();
				sp[-2] = _mia_vm_read(type, _mia_vm_memory(vm, sp[-2].u, sp[-1].u, mia_code_size(type)));
				sp -= 1;
				_mia_vm_next();
			}
			_mia_vm_op(store_loc) {
				const mia_code_e type = exec->instruction->type;
				const uint64_t handle = sp[-3].u;
				_mia_vm_save();
				uint8_t* const memory = _mia_vm_memory(vm, handle, sp[-2].u, mia_code_size(type));
				if (handle & _mia_vm_handle_read_only) _mia_vm_fail(vm, "write through a read-only slice");
				_mia_vm_write(type, memory, sp[-1]);
				sp -= 3;
				_mia_vm_next();
			}
			_mia_vm_op(index_slice) {
				const mia_code_e type = exec->instruction->type;
				const _mia_vm_word_u index = sp[-1];
				const uint64_t handle = sp[-3].u, slice = sp[-2].u;
				_mia_vm_save();
				if (index.u >= _mia_vm_slice_length(slice)) _mia_vm_out_of_bounds(vm, exec->instruction->target, index, _mia_vm_slice_length(slice), true);
				const uint64_t size = mia_code_size(type);
				sp -= 3;
				*sp++ = _mia_vm_read(type, _mia_vm_memory(vm, handle, _mia_vm_slice_offset(slice) + index.u * size, size));
				_mia_vm_next();
			}
			// a run of instructions as one operation: every instruction is counted, and with fewer steps left
			// than the run has, the first runs alone (its plain operation). an error stops at the instruction
			// that has it (pc moves past the run first, so the trace names it).
#define _mia_vm_fused(_count)                                                  \
			do {                                                               \
				if (stop_at - steps < (uint64_t)(_count) - 1) _mia_vm_plain(); \
				steps += (_count) - 1;                                         \
				pc += (_count) - 1;                                            \
			} while (0)
			_mia_vm_op(load_const) {
				_mia_vm_fused(2);
				memcpy(sp, base + exec->a, 8);
				sp[1].u = exec[1].word;
				sp += 2;
				_mia_vm_next();
			}
			_mia_vm_op(load_load) {
				_mia_vm_fused(2);
				memcpy(sp, base + exec->a, 8);
				memcpy(sp + 1, base + exec[1].a, 8);
				sp += 2;
				_mia_vm_next();
			}
			_mia_vm_op(add_const)
			_mia_vm_op(sub_const) {
				_mia_vm_fused(3);
				uint64_t value;
				memcpy(&value, base + exec->a, 8);
				(sp++)->u = (_mia_vm_x_add_const == exec->x) ? value + exec[1].word : value - exec[1].word;
				_mia_vm_next();
			}
			_mia_vm_op(add_const_store)
			_mia_vm_op(sub_const_store) {
				_mia_vm_fused(4);
				uint64_t value;
				memcpy(&value, base + exec->a, 8);
				value = (_mia_vm_x_add_const_store == exec->x) ? value + exec[1].word : value - exec[1].word;
				memcpy(base + exec[3].a, &value, 8);
				_mia_vm_next();
			}
			_mia_vm_op(index_local8)
			_mia_vm_op(index_local1) {
				_mia_vm_fused(3);
				uint64_t slice[2];
				_mia_vm_word_u index;
				memcpy(slice, base + exec->a, 16);
				memcpy(&index, base + exec[1].a, 8);
				_mia_vm_save();
				if (index.u >= _mia_vm_slice_length(slice[1])) _mia_vm_out_of_bounds(vm, exec[2].instruction->target, index, _mia_vm_slice_length(slice[1]), true);
				if (_mia_vm_x_index_local8 == exec->x) {
					memcpy(sp, _mia_vm_memory(vm, slice[0], _mia_vm_slice_offset(slice[1]) + index.u * 8, 8), 8);
				}
				else {
					sp->u = *_mia_vm_memory(vm, slice[0], _mia_vm_slice_offset(slice[1]) + index.u, 1);
					if (mia_code_bool == exec[2].instruction->type) sp->u = sp->u != 0;
				}
				sp += 1;
				_mia_vm_next();
			}
			_mia_vm_op(loc_index_local) {
				_mia_vm_fused(3);
				uint64_t slice[2];
				_mia_vm_word_u index;
				memcpy(slice, base + exec->a, 16);
				memcpy(&index, base + exec[1].a, 8);
				_mia_vm_save();
				if (index.u >= _mia_vm_slice_length(slice[1])) _mia_vm_out_of_bounds(vm, exec[2].instruction->type, index, _mia_vm_slice_length(slice[1]), true);
				const uint64_t offset = _mia_vm_slice_offset(slice[1]) + index.u * (uint64_t)exec[2].a;
				(void)_mia_vm_memory(vm, slice[0], offset, (uint64_t)exec[2].a);
				sp[0].u = slice[0];
				sp[1].u = offset;
				sp += 2;
				_mia_vm_next();
			}
#undef _mia_vm_fused
			// a comparison, then its jump: both instructions are counted, and a limit between them stops
			// where the two would have (the bool left on the stack then is never read)
#define _mia_vm_compare_jump(_name, _test)                                     \
			_mia_vm_op(_name##_jump) {                                         \
				sp -= 2;                                                       \
				const bool holds = (_test);                                    \
				if (steps >= stop_at) {                                        \
					(sp++)->u = holds;                                         \
					_mia_vm_next();                                            \
				}                                                              \
				steps += 1;                                                    \
				pc = holds ? pc + 1 : exec->b;                                 \
				_mia_vm_next();                                                \
			}
			_mia_vm_compare_jump(eq, sp[0].u == sp[1].u)
			_mia_vm_compare_jump(ne, sp[0].u != sp[1].u)
			_mia_vm_compare_jump(slt, sp[0].i < sp[1].i)
			_mia_vm_compare_jump(sle, sp[0].i <= sp[1].i)
			_mia_vm_compare_jump(sgt, sp[0].i > sp[1].i)
			_mia_vm_compare_jump(sge, sp[0].i >= sp[1].i)
			_mia_vm_compare_jump(ult, sp[0].u < sp[1].u)
			_mia_vm_compare_jump(ule, sp[0].u <= sp[1].u)
			_mia_vm_compare_jump(ugt, sp[0].u > sp[1].u)
			_mia_vm_compare_jump(uge, sp[0].u >= sp[1].u)
#undef _mia_vm_compare_jump
			_mia_vm_op(call)
				_mia_vm_save();
				_mia_vm_call(vm, exec->a);
				_mia_vm_load();
				_mia_vm_next();
			_mia_vm_op(return)
				_mia_vm_save();
				_mia_vm_return(vm, exec->a);
				_mia_vm_load();
				_mia_vm_next();
			_mia_vm_op(step)
				_mia_vm_save();
				_mia_vm_step(vm, frame, exec->instruction);
				_mia_vm_load();
				_mia_vm_next();
#if _mia_vm_threaded
	}
stopped:
	_mia_vm_save();
	_mia_vm_fail(vm, "stopped after %llu instructions", (unsigned long long)stop_at);
#else
			case _mia_vm_x_count: mia_unreachable();
		}
	}
#endif
#undef _mia_vm_op
#undef _mia_vm_next
#undef _mia_vm_plain
#undef _mia_vm_save
#undef _mia_vm_load
}

#if _mia_vm_threaded
#	pragma GCC diagnostic pop
#endif

// --- loading -------------------------------------------------------------------------------

/// the fast operation for an instruction (the general step when there is none).
static uint8_t _mia_vm_decode(const _mia_vm_s* const vm, const mia_bytecode_instruction_s* const instruction, uint64_t* const word) {
	const mia_code_e code = instruction->type;
	const bool wide = mia_code_s64 == code || mia_code_u64 == code || mia_code_int == code || mia_code_uint == code;
	const bool is_double = mia_code_f64 == code || mia_code_float == code;
	const bool integer = code <= mia_code_uint || mia_code_bool == code || mia_code_fn == code;
	const bool is_signed = mia_code_is_signed(code), is_unsigned = integer && !is_signed;
	if (mia_opcode_CONST == instruction->opcode) {
		const _mia_vm_constant_s* const constant = &vm->constants.data[instruction->a];
		if (constant->count != 1) return _mia_vm_x_const;
		*word = constant->words[0].u;
		return _mia_vm_x_const1;
	}
	switch (instruction->opcode) {
		case mia_opcode_ZERO:  return _mia_vm_x_zero;
		case mia_opcode_LOAD:
			if (mia_code_bytes == code) return (instruction->b > 0) ? _mia_vm_x_load_bytes : _mia_vm_x_step;
			return (wide || is_double || mia_code_fn == code) ? _mia_vm_x_load8 : (mia_code_slice == code) ? _mia_vm_x_load16 : _mia_vm_x_load;
		case mia_opcode_STORE:
			if (mia_code_bytes == code) return _mia_vm_x_store_bytes;
			return (wide || is_double || mia_code_fn == code) ? _mia_vm_x_store8 : (mia_code_slice == code) ? _mia_vm_x_store16 : _mia_vm_x_store;
		case mia_opcode_FIELD:       return _mia_vm_as_bytes(code) ? _mia_vm_x_step : _mia_vm_x_field;
		case mia_opcode_SET_FIELD:   return _mia_vm_as_bytes(code) ? _mia_vm_x_step : _mia_vm_x_set_field;
		case mia_opcode_NOT:         return _mia_vm_x_not;
		case mia_opcode_DUP:   return (1 == instruction->a) ? _mia_vm_x_dup1 : _mia_vm_x_step;
		case mia_opcode_POP:   return _mia_vm_x_pop;
		case mia_opcode_ADD:   return wide ? _mia_vm_x_add : is_double ? _mia_vm_x_fadd : (mia_code_f32 == code) ? _mia_vm_x_f32add : _mia_vm_x_step;
		case mia_opcode_SUB:   return wide ? _mia_vm_x_sub : is_double ? _mia_vm_x_fsub : (mia_code_f32 == code) ? _mia_vm_x_f32sub : _mia_vm_x_step;
		case mia_opcode_MUL:   return wide ? _mia_vm_x_mul : is_double ? _mia_vm_x_fmul : (mia_code_f32 == code) ? _mia_vm_x_f32mul : _mia_vm_x_step;
		case mia_opcode_EQ:    return integer ? _mia_vm_x_eq : _mia_vm_x_step;
		case mia_opcode_NE:    return integer ? _mia_vm_x_ne : _mia_vm_x_step;
		case mia_opcode_LT:    return is_signed ? _mia_vm_x_slt : is_unsigned ? _mia_vm_x_ult : _mia_vm_x_flt;
		case mia_opcode_LE:    return is_signed ? _mia_vm_x_sle : is_unsigned ? _mia_vm_x_ule : _mia_vm_x_fle;
		case mia_opcode_GT:    return is_signed ? _mia_vm_x_sgt : is_unsigned ? _mia_vm_x_ugt : _mia_vm_x_fgt;
		case mia_opcode_GE:    return is_signed ? _mia_vm_x_sge : is_unsigned ? _mia_vm_x_uge : _mia_vm_x_fge;
		case mia_opcode_JUMP:  return _mia_vm_x_jump;
		case mia_opcode_JUMP_IF_FALSE: return _mia_vm_x_jump_false;
		case mia_opcode_JUMP_IF_TRUE:  return _mia_vm_x_jump_true;
		case mia_opcode_RANGE_TEST:    return _mia_vm_x_range_test;
		case mia_opcode_RANGE_NEXT:    return _mia_vm_x_range_next;
		case mia_opcode_INDEX_SLICE:
			return (wide || is_double || mia_code_fn == code) ? _mia_vm_x_index_slice8 : (mia_code_u8 == code || mia_code_bool == code) ? _mia_vm_x_index_slice1
				: _mia_vm_as_bytes(code) ? _mia_vm_x_step : _mia_vm_x_index_slice;
		case mia_opcode_LOC_INDEX_SLICE: return _mia_vm_x_loc_index_slice;
		case mia_opcode_LOAD_LOC:  return (wide || is_double || mia_code_fn == code) ? _mia_vm_x_load_loc8 : _mia_vm_as_bytes(code) ? _mia_vm_x_step : _mia_vm_x_load_loc;
		case mia_opcode_STORE_LOC: return (wide || is_double || mia_code_fn == code) ? _mia_vm_x_store_loc8 : _mia_vm_as_bytes(code) ? _mia_vm_x_step : _mia_vm_x_store_loc;
		case mia_opcode_CALL:   return _mia_vm_x_call;
		case mia_opcode_RETURN: return _mia_vm_x_return;
		default:           return _mia_vm_x_step;
	}
}

/// what an instruction takes from and leaves on the operand stack (words);
/// false when its operands are out of range.
static bool _mia_vm_effect(const _mia_vm_s* const vm, const mia_bytecode_function_s* const function, const mia_bytecode_instruction_s* const instruction,
						   uint64_t* const pop, uint64_t* const push) {
	const int64_t a = instruction->a, b = instruction->b, c = instruction->c;
	const mia_code_e code = instruction->type, second = instruction->target;
	const int64_t limit = (int64_t)1 << 40;
	const uint64_t frame = function->frame;
	*pop = 0;
	*push = 0;
	if (a < -limit || a > limit || b < -limit || b > limit || c < -limit || c > limit) return false;
	// the size of the value an instruction moves is its last operand: the
	// size of its type code, or any size for BYTES
	const int operands = mia_opcode_operands(instruction->opcode);
	const int64_t size = (1 == operands) ? a : (2 == operands) ? b : c;
	const bool sized = (mia_code_bytes == code) ? (size >= 0 && size <= ((int64_t)1 << 32)) : (size >= 0 && (uint64_t)size == mia_code_size(code));
	const uint64_t words = _mia_vm_value_words(code, (uint64_t)size);
	const bool scalar = code < mia_code_slice, integer = code <= mia_code_uint, index = second <= mia_code_uint;
#define _mia_vm_in_frame(_offset, _size) ((_offset) >= 0 && (_size) >= 0 && (uint64_t)(_offset) + (uint64_t)(_size) <= frame)
	switch (instruction->opcode) {
		case mia_opcode_CONST:      *push = vm->constants.data[a].count; return true;
		case mia_opcode_ZERO:       *push = (uint64_t)a; return a >= 0;
		case mia_opcode_POP:        *pop = (uint64_t)a; return a >= 0;
		case mia_opcode_DUP:        *pop = (uint64_t)a; *push = 2 * (uint64_t)a; return a >= 0;
		case mia_opcode_LOAD:       *push = words; return sized && _mia_vm_in_frame(a, size);
		case mia_opcode_STORE:      *pop = words; return sized && _mia_vm_in_frame(a, size);
		case mia_opcode_LOC_LOCAL:  *push = 2; return _mia_vm_in_frame(a, 0);
		case mia_opcode_LOC_CONST:  *push = 2; return true;
		case mia_opcode_LOC_FIELD:  *pop = 2; *push = 2; return a >= 0;
		case mia_opcode_LOC_INDEX:  *pop = 3; *push = 2; return integer && a >= 0 && b >= 0;
		case mia_opcode_LOC_INDEX_SLICE: *pop = 3; *push = 2; return integer && a >= 0;
		case mia_opcode_TEMP_LOC:   *pop = words; *push = 2; return sized && _mia_vm_in_frame(a, size);
		case mia_opcode_LOAD_LOC:   *pop = 2; *push = words; return sized;
		case mia_opcode_STORE_LOC:  *pop = 2 + words; return sized;
		case mia_opcode_FIELD:      *pop = (uint64_t)b; *push = words; return sized && a >= 0 && b >= 0 && a + size <= b * 8;
		case mia_opcode_SET_FIELD:  *pop = (uint64_t)b + words; *push = (uint64_t)b; return sized && a >= 0 && b >= 0 && a + size <= b * 8;
		case mia_opcode_MAKE_ARRAY:
			*pop = (uint64_t)a * words;
			*push = _mia_vm_words((uint64_t)a * (uint64_t)size);
			return sized && a >= 0 && a <= ((int64_t)1 << 28);
		case mia_opcode_INDEX:       *pop = (uint64_t)b + 1; *push = words; return sized && index && a >= 0 && b >= 0 && (uint64_t)a * (uint64_t)size <= (uint64_t)b * 8;
		case mia_opcode_INDEX_SLICE: *pop = 3; *push = words; return sized && index;
		case mia_opcode_LEN_SLICE:
		case mia_opcode_SLICE_IS_NULL: *pop = 2; *push = 1; return true;
		case mia_opcode_SLICE_LOC:
		case mia_opcode_SLICE_SLICE: {
			*pop = 2 + ((a & mia_bytecode_slice_low) ? 1 : 0) + ((a & mia_bytecode_slice_high) ? 1 : 0);
			*push = 2;
			return b >= 0 && c >= 0;
		}
		case mia_opcode_JUMP:      return true;
		case mia_opcode_JUMP_IF_FALSE:
		case mia_opcode_JUMP_IF_TRUE: *pop = 1; return true;
		case mia_opcode_NOT:       *pop = 1; *push = 1; return true;
		case mia_opcode_RANGE_INIT: *pop = 2; return integer && _mia_vm_in_frame(a, 24);
		case mia_opcode_RANGE_TEST:
		case mia_opcode_RANGE_NEXT:
		case mia_opcode_RANGE_STEP: return _mia_vm_in_frame(a, 24);
		case mia_opcode_CALL:
			*pop = vm->prepared.data[a].arguments;
			*push = vm->module->functions.data[a].result;
			return true;
		case mia_opcode_CALL_VALUE: *pop = (uint64_t)a + 1; *push = (uint64_t)b; return a >= 0 && b >= 0;
		case mia_opcode_CALL_INTRINSIC: {
			const mia_bytecode_intrinsic_s* const intrinsic = &vm->module->intrinsics.data[a];
			*pop = (uint64_t)b;
			*push = _mia_vm_intrinsic_result(vm, intrinsic);
			return b >= 0 && _mia_vm_intrinsic_arguments(vm, intrinsic) == (uint64_t)b && *push <= 2;
		}
		case mia_opcode_FUNCTION:
		case mia_opcode_INTRINSIC: *push = 1; return true;
		case mia_opcode_RETURN:    *pop = (uint64_t)a; return a >= 0 && (uint64_t)a == function->result;
		case mia_opcode_ASSERT_FAIL: return true;
		case mia_opcode_ADD: case mia_opcode_SUB: case mia_opcode_MUL: case mia_opcode_DIV: case mia_opcode_MOD: case mia_opcode_AND: case mia_opcode_OR: case mia_opcode_XOR:
		case mia_opcode_SHL: case mia_opcode_SHR: case mia_opcode_EQ: case mia_opcode_NE: case mia_opcode_LT: case mia_opcode_GT: case mia_opcode_LE: case mia_opcode_GE:
			*pop = 2;
			*push = 1;
			return scalar && ((mia_opcode_SHL != instruction->opcode && mia_opcode_SHR != instruction->opcode) || index);
		case mia_opcode_NEG: case mia_opcode_BITNOT: case mia_opcode_CAST:
			*pop = 1;
			*push = 1;
			return scalar && (mia_opcode_CAST != instruction->opcode || second < mia_code_slice);
		case mia_opcode_count: return false;
	}
#undef _mia_vm_in_frame
	return false;
}

/// checks a function (see the top of this file) and decodes it.
/// fuses the runs of instructions the profiles of programs show most (see
/// _mia_vm_x_e), each into the operation of its first instruction, when
/// nothing jumps into the run. 'target' marks the instructions jumps go to.
static void _mia_vm_fuse(_mia_vm_exec_s* const code, const size_t count, const bool* const target) {
	for (size_t i = 0; i + 1 < count; ++i) {
		_mia_vm_exec_s* const exec = &code[i];
		// (a comparison fused with its jump is left as it is)
		if (exec->x != exec->plain || target[i + 1]) continue;
		const uint8_t second = code[i + 1].plain;
		const uint8_t third = (i + 2 < count && !target[i + 2]) ? code[i + 2].plain : (uint8_t)_mia_vm_x_count;
		const uint8_t fourth = (i + 3 < count && _mia_vm_x_count != third && !target[i + 3]) ? code[i + 3].plain : (uint8_t)_mia_vm_x_count;
		const bool add = _mia_vm_x_add == third, sub = _mia_vm_x_sub == third;
		uint8_t fused = _mia_vm_x_count;
		size_t length = 0;
		if (_mia_vm_x_load8 == exec->x && _mia_vm_x_const1 == second) {
			if ((add || sub) && _mia_vm_x_store8 == fourth) {
				fused = add ? _mia_vm_x_add_const_store : _mia_vm_x_sub_const_store;
				length = 4;
			}
			else if (add || sub) {
				fused = add ? _mia_vm_x_add_const : _mia_vm_x_sub_const;
				length = 3;
			}
			else {
				fused = _mia_vm_x_load_const;
				length = 2;
			}
		}
		else if (_mia_vm_x_load8 == exec->x && _mia_vm_x_load8 == second) {
			fused = _mia_vm_x_load_load;
			length = 2;
		}
		else if (_mia_vm_x_load16 == exec->x && _mia_vm_x_load8 == second) {
			length = 3;
			if (_mia_vm_x_index_slice8 == third) fused = _mia_vm_x_index_local8;
			else if (_mia_vm_x_index_slice1 == third) fused = _mia_vm_x_index_local1;
			else if (_mia_vm_x_loc_index_slice == third) fused = _mia_vm_x_loc_index_local;
		}
		if (_mia_vm_x_count == fused) continue;
		exec->x = fused;
		i += length - 1;
	}
}

static void _mia_vm_verify(_mia_vm_s* const vm, const int64_t index) {
	const mia_bytecode_function_s* const function = &vm->module->functions.data[index];
	_mia_vm_prepared_s* const prepared = &vm->prepared.data[index];
	const size_t count = function->code.count;
	if (0 == count) _mia_vm_fail(vm, "the module is broken (`%s` has no code)", function->name);
	int64_t* const depth = mia_malloc(count * sizeof(int64_t));
	for (size_t i = 0; i < count; ++i) depth[i] = -1;
	mia_vector_of(size_t) work = {0};
	depth[0] = 0;
	mia_vector_push(&work, (size_t)0);
	uint64_t highest = 0;
	const char* problem = NULL;
	while (work.count > 0 && NULL == problem) {
		const size_t at = mia_vector_pop(&work);
		const mia_bytecode_instruction_s* const instruction = &function->code.data[at];
		uint64_t pop = 0, push = 0;
		if (!_mia_vm_effect(vm, function, instruction, &pop, &push)) {
			problem = "an operand is out of range";
			break;
		}
		const uint64_t before = (uint64_t)depth[at];
		if (before < pop) {
			problem = "the operand stack underflows";
			break;
		}
		const uint64_t after = before - pop + push;
		if (after > highest) highest = after;
		if (before > highest) highest = before;
		if (after > ((uint64_t)1 << 30)) {
			problem = "the operand stack grows too large";
			break;
		}
		if (mia_opcode_RETURN == instruction->opcode && before != pop) {
			problem = "a return leaves values on the stack";
			break;
		}
		size_t next[2];
		size_t successors = 0;
		const mia_opcode_e opcode = instruction->opcode;
		if (mia_opcode_JUMP == opcode || mia_opcode_JUMP_IF_FALSE == opcode || mia_opcode_JUMP_IF_TRUE == opcode) next[successors++] = (size_t)instruction->a;
		if (mia_opcode_RANGE_TEST == opcode || mia_opcode_RANGE_NEXT == opcode) next[successors++] = (size_t)instruction->b;
		if (opcode != mia_opcode_JUMP && opcode != mia_opcode_RETURN && opcode != mia_opcode_ASSERT_FAIL) next[successors++] = at + 1;
		for (size_t s = 0; s < successors; ++s) {
			if (next[s] >= count) {
				problem = "the code runs past its end";
				break;
			}
			if (depth[next[s]] < 0) {
				depth[next[s]] = (int64_t)after;
				mia_vector_push(&work, next[s]);
			}
			else if ((uint64_t)depth[next[s]] != after) {
				problem = "the operand stack differs between paths";
				break;
			}
		}
	}
	mia_vector_free(&work);
	mia_free(depth);
	if (problem != NULL) _mia_vm_fail(vm, "the module is broken (`%s`: %s)", function->name, problem);
	prepared->max_stack = (size_t)highest;
	prepared->frame = (size_t)((function->frame + 15) / 16 * 16);
	prepared->code = mia_malloc(count * sizeof(_mia_vm_exec_s));
	for (size_t i = 0; i < count; ++i) {
		const mia_bytecode_instruction_s* const instruction = &function->code.data[i];
		_mia_vm_exec_s* const exec = &prepared->code[i];
		*exec = (_mia_vm_exec_s) { .instruction = instruction, .a = instruction->a, .b = instruction->b, };
		exec->x = _mia_vm_decode(vm, instruction, &exec->word);
		exec->plain = exec->x;
	}
	// an integer comparison and the jump after it become one operation, unless something jumps to the
	// jump (then it has to stay an instruction of its own); before a jump_true, the operation tests the
	// opposite comparison (and jumps when that one does not hold)
	bool* const target = mia_calloc(count + 1, sizeof(bool));
	for (size_t i = 0; i < count; ++i) {
		const mia_bytecode_instruction_s* const instruction = &function->code.data[i];
		const bool jumps = mia_opcode_JUMP == instruction->opcode || mia_opcode_JUMP_IF_FALSE == instruction->opcode
			|| mia_opcode_JUMP_IF_TRUE == instruction->opcode;
		const bool ranges = mia_opcode_RANGE_TEST == instruction->opcode || mia_opcode_RANGE_NEXT == instruction->opcode;
		const int64_t to = jumps ? instruction->a : ranges ? instruction->b : -1;
		if (to >= 0 && (uint64_t)to <= count) target[to] = true;
	}
	for (size_t i = 0; i + 1 < count; ++i) {
		const uint8_t jump = prepared->code[i + 1].x;
		if ((jump != _mia_vm_x_jump_false && jump != _mia_vm_x_jump_true) || target[i + 1]) continue;
		const bool opposite = _mia_vm_x_jump_true == jump;
		uint8_t fused = _mia_vm_x_count;
		switch (prepared->code[i].x) {
			case _mia_vm_x_eq: fused = opposite ? _mia_vm_x_ne_jump : _mia_vm_x_eq_jump; break;
			case _mia_vm_x_ne: fused = opposite ? _mia_vm_x_eq_jump : _mia_vm_x_ne_jump; break;
			case _mia_vm_x_slt: fused = opposite ? _mia_vm_x_sge_jump : _mia_vm_x_slt_jump; break;
			case _mia_vm_x_sle: fused = opposite ? _mia_vm_x_sgt_jump : _mia_vm_x_sle_jump; break;
			case _mia_vm_x_sgt: fused = opposite ? _mia_vm_x_sle_jump : _mia_vm_x_sgt_jump; break;
			case _mia_vm_x_sge: fused = opposite ? _mia_vm_x_slt_jump : _mia_vm_x_sge_jump; break;
			case _mia_vm_x_ult: fused = opposite ? _mia_vm_x_uge_jump : _mia_vm_x_ult_jump; break;
			case _mia_vm_x_ule: fused = opposite ? _mia_vm_x_ugt_jump : _mia_vm_x_ule_jump; break;
			case _mia_vm_x_ugt: fused = opposite ? _mia_vm_x_ule_jump : _mia_vm_x_ugt_jump; break;
			case _mia_vm_x_uge: fused = opposite ? _mia_vm_x_ult_jump : _mia_vm_x_uge_jump; break;
			default: break;
		}
		if (_mia_vm_x_count == fused) continue;
		prepared->code[i].x = fused;
		prepared->code[i].b = prepared->code[i + 1].a;
	}
	_mia_vm_fuse(prepared->code, count, target);
	mia_free(target);
}

static void _mia_vm_load(_mia_vm_s* const vm) {
	const mia_bytecode_module_s* const module = vm->module;
	_mia_vm_load_constants(vm);
	_mia_vm_link(vm, (vm->options->plugins != NULL) ? vm->options->plugins : mia_plugins_default());
	if (module->entry < 0 || (size_t)module->entry >= module->functions.count) _mia_vm_fail(vm, "the module has no entry function");
	// the words of every function's arguments first: calls are checked against them
	for (size_t i = 0; i < module->functions.count; ++i) {
		const mia_bytecode_function_s* const function = &module->functions.data[i];
		if (function->frame > mia_bytecode_frame_size_limit) _mia_vm_fail(vm, "the module is broken (the frame of `%s` is too large)", function->name);
		_mia_vm_prepared_s prepared = {0};
		for (size_t s = 0; s < function->slots.count; ++s) {
			const mia_bytecode_slot_s* const slot = &function->slots.data[s];
			const uint64_t size = (mia_code_bytes == slot->code) ? slot->size : mia_code_size(slot->code);
			if (slot->offset > function->frame || size > function->frame - slot->offset || slot->size > function->frame) {
				_mia_vm_fail(vm, "the module is broken (a parameter of `%s` is outside its frame)", function->name);
			}
			prepared.arguments += _mia_vm_value_words(slot->code, slot->size);
		}
		prepared.packed = true;
		for (size_t s = 0; s < function->slots.count; ++s) {
			const mia_code_e code = function->slots.data[s].code;
			const bool word = mia_code_s64 == code || mia_code_u64 == code || mia_code_int == code || mia_code_uint == code || mia_code_fn == code
				|| mia_code_f64 == code || mia_code_float == code;
			prepared.packed = prepared.packed && word && function->slots.data[s].offset == 8 * s;
		}
		mia_vector_push(&vm->prepared, prepared);
	}
	for (size_t i = 0; i < module->functions.count; ++i) _mia_vm_verify(vm, (int64_t)i);
	const mia_bytecode_function_s* const entry = &module->functions.data[module->entry];
	const bool command_line = 1 == entry->slots.count && mia_code_slice == entry->slots.data[0].code;
	if (vm->prepared.data[module->entry].arguments != 0 && !command_line) _mia_vm_fail(vm, "the module is broken (the entry function takes arguments)");
}

/// the command line as a [][]u8 on the stack: read-only blocks the program
/// cannot free (they are not heap memory).
static void _mia_vm_push_command_line(_mia_vm_s* const vm) {
	const size_t count = vm->options->argument_count;
	uint64_t bytes = 0;
	for (size_t i = 0; i < count; ++i) bytes += strlen(vm->options->arguments[i]);
	uint8_t* const text = mia_calloc(1, bytes > 0 ? (size_t)bytes : 1);
	const uint64_t text_handle = _mia_vm_block_new(vm, _mia_vm_block_static, text, bytes, mia_free) | _mia_vm_handle_read_only;
	uint64_t* const slices = mia_calloc(count > 0 ? count : 1, 16);
	const uint64_t slices_handle = _mia_vm_block_new(vm, _mia_vm_block_static, (uint8_t*)slices, 16 * count, mia_free) | _mia_vm_handle_read_only;
	uint64_t offset = 0;
	for (size_t i = 0; i < count; ++i) {
		const size_t length = strlen(vm->options->arguments[i]);
		memcpy(text + offset, vm->options->arguments[i], length);
		slices[2 * i] = text_handle;
		slices[2 * i + 1] = _mia_vm_slice_word(offset, length);
		offset += length;
	}
	vm->stack[vm->sp++].u = (count > 0) ? slices_handle : 0;
	vm->stack[vm->sp++].u = _mia_vm_slice_word(0, count);
}

static void _mia_vm_start(_mia_vm_s* const vm) {
	_mia_vm_load(vm);
	_mia_vm_reserve(vm, 1024);
	if (vm->prepared.data[vm->module->entry].arguments > 0) _mia_vm_push_command_line(vm);
	_mia_vm_call(vm, vm->module->entry);
	mia_vm_profile_s* const profile = vm->options->profile;
	if (NULL == profile) {
		_mia_vm_loop(vm);
		return;
	}
	mia_vm_profile_free(profile);
	for (size_t i = 0; i < vm->module->functions.count; ++i) {
		const size_t count = vm->module->functions.data[i].code.count;
		const mia_vm_profile_function_s counts = {
			.count = count, .runs = mia_calloc(count > 0 ? count : 1, sizeof(uint64_t)), .followed = mia_calloc(count > 0 ? count : 1, sizeof(uint64_t)),
		};
		mia_vector_push(&profile->functions, counts);
	}
	_mia_vm_profile_loop(vm);
}

bool mia_vm_run(const mia_bytecode_module_s* const module, const mia_vm_options_s* const options, mia_vm_result_s* const result) {
	*result = (mia_vm_result_s) {0};
	const mia_vm_options_s defaults = {0};
	jmp_buf stop;
	_mia_vm_s vm = { .module = module, .options = (options != NULL) ? options : &defaults, .stop = &stop, };
	vm.out = (vm.options->out != NULL) ? vm.options->out : stdout;
	vm.call_depth = (vm.options->call_depth > 0) ? vm.options->call_depth : mia_vm_call_depth_default;
	if (0 == setjmp(stop)) _mia_vm_start(&vm);
	(void)fflush(vm.out);
	result->steps = vm.steps;
	result->allocations = vm.allocations;
	if (vm.error != NULL) {
		result->failed = true;
		result->message = vm.error;
		for (size_t i = vm.frames.count; i-- > 0;) {
			const _mia_vm_frame_s* const frame = &vm.frames.data[i];
			const mia_bytecode_function_s* const function = &module->functions.data[frame->function];
			const size_t index = (frame->pc > 0) ? (size_t)frame->pc - 1 : 0;
			mia_vm_trace_entry_s entry = { .function = function->name, };
			if (index < function->positions.count && function->positions.data[index].file >= 0) {
				entry.has_position = true;
				entry.position = function->positions.data[index];
				if ((size_t)entry.position.file < module->files.count) entry.path = module->files.data[entry.position.file];
				else entry.has_position = false;
			}
			mia_vector_push(&result->trace, entry);
		}
	}
	else if (vm.returned && module->entry_returns) {
		result->has_result = true;
		result->result = vm.result.i;
	}
	_mia_vm_free_memory(&vm);
	for (size_t i = 0; i < vm.prepared.count; ++i) mia_free(vm.prepared.data[i].code);
	mia_vector_free(&vm.prepared);
	mia_vector_free(&vm.frames);
	_mia_vm_unlink(&vm);
	mia_string_free(&vm.buffer);
	mia_string_free(&vm.bytes);
	return !result->failed;
}

void mia_vm_result_free(mia_vm_result_s* const result) {
	mia_free(result->message);
	mia_vector_free(&result->trace);
	*result = (mia_vm_result_s) {0};
}

void mia_vm_profile_free(mia_vm_profile_s* const profile) {
	for (size_t i = 0; i < profile->functions.count; ++i) {
		mia_free(profile->functions.data[i].runs);
		mia_free(profile->functions.data[i].followed);
	}
	mia_vector_free(&profile->functions);
}

int mia_vm_exit_status(const mia_vm_result_s* const result) {
	return result->has_result ? (int)(result->result & 0xFF) : 0;
}
