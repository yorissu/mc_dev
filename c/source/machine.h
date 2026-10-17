
#ifndef __mc__machine_h__
#define __mc__machine_h__

/// the virtual machine's internals, shared by its source files:
///
///   vm.c           loading a module, the instruction loop, runtime errors
///   vm_memory.c    objects, the garbage collector, copying, locations, slices
///   vm_builtins.c  builtins, natives and printing values
///
/// heap objects (lists, blocks, locations and frame records) start with an
/// mc_object_s and are linked into one list that the collector sweeps. the
/// collector runs only between instructions, when every live object is
/// reachable from the stack, the frames or the constants.

#include "mc/vm.h"

#include <setjmp.h>

typedef enum {
	mc_value_none    ,
	mc_value_integer ,
	mc_value_real    ,
	mc_value_boolean ,
	mc_value_list    ,   // an array or a struct
	mc_value_slice   ,
	mc_value_location,
	mc_value_function,
} mc_value_kind_e;

typedef enum {
	mc_object_list    ,
	mc_object_block   ,
	mc_object_location,
	mc_object_frame   ,
} mc_object_kind_e;

typedef struct mc_object_s mc_object_s;

struct mc_object_s {
	mc_object_s*     next  ;
	mc_object_kind_e kind  ;
	bool             marked;
};

typedef struct mc_list_s mc_list_s;
typedef struct mc_block_s mc_block_s;
typedef struct mc_place_s mc_place_s;

typedef struct {
	mc_value_kind_e kind;
	union {
		__int128    integer;
		double      real   ;
		bool        boolean;
		mc_list_s*  list   ;
		struct { mc_block_s* block; int64_t start; int64_t length; bool read_only; } slice;   // block null: null
		mc_place_s* location;
		struct { bool builtin; int64_t index; } function;
	} as;
} mc_value_s;

struct mc_list_s {
	mc_object_s object;
	size_t      count ;
	mc_value_s* items ;
};

typedef enum {
	mc_block_heap  ,   // from alloc, until free or realloc
	mc_block_stack ,   // an array of a call, or inside another block
	mc_block_static,   // constants
} mc_block_kind_e;

/// memory that slices view. 'owner' is the frame or block it lives in (null:
/// it lives as long as the program).
struct mc_block_s {
	mc_object_s     object;
	mc_list_s*      data  ;
	mc_object_s*    owner ;
	mc_block_kind_e kind  ;
	bool            freed ;
};

/// a place: an assignment target or a reference argument. 'container' holds
/// the value at 'key'.
struct mc_place_s {
	mc_object_s  object   ;
	mc_list_s*   container;
	int64_t      key      ;
	mc_object_s* owner    ;
	mc_block_s*  block    ;
	bool         read_only;
};

/// what outlives a call: its locals and whether it is still running.
typedef struct {
	mc_object_s object;
	mc_list_s*  locals;
	bool        alive ;
} mc_frame_record_s;

typedef struct {
	int64_t            function;
	int64_t            pc      ;
	mc_frame_record_s* record  ;
} mc_frame_s;

/// a type descriptor, decoded.
typedef enum {
	mc_descriptor_int, mc_descriptor_float, mc_descriptor_bool, mc_descriptor_array,
	mc_descriptor_slice, mc_descriptor_struct, mc_descriptor_function,
} mc_descriptor_kind_e;

typedef struct mc_vm_s mc_vm_s;

typedef mc_value_s (*mc_builtin_f)(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count, const mc_bc_builtin_s* const builtin);

typedef mc_value_s (*mc_native_f)(mc_vm_s* const vm, mc_value_s* const arguments, const size_t count);

struct mc_vm_s {
	const mc_module_bc_s*        module     ;
	const mc_vm_options_s*       options    ;
	FILE*                        out        ;
	mc_vector_of(mc_value_s)     stack      ;
	mc_vector_of(mc_frame_s)     frames     ;
	mc_vector_of(mc_value_s)     constants  ;   // the module's constants as values
	mc_vector_of(mc_descriptor_kind_e) kinds;   // of the module's types
	mc_vector_of(mc_builtin_f)   builtins   ;   // of the module's builtins
	mc_vector_of(mc_native_f)    natives    ;   // of the module's natives
	mc_value_s                   result     ;
	mc_vector_of(mc_value_s)     arguments  ;   // of the builtin or native being called
	mc_string_s                  buffer     ;   // what a builtin or native prints
	mc_string_s                  bytes      ;
	// the collector
	mc_object_s*                 objects    ;
	size_t                       allocated  ;   // objects allocated since the last collection
	size_t                       live       ;   // objects that survived it
	size_t                       threshold  ;
	uint64_t                     collections;
	uint64_t                     steps      ;   // instructions executed
	mc_vector_of(mc_object_s*)   gray       ;
	// stopping
	jmp_buf*                     stop       ;
	char*                        error      ;
};

#define mc_call_depth_limit 10000

// vm.c
/// stops the program with a runtime error.
mc_noreturn void mc_vm_fail(mc_vm_s* const vm, const char* const format, ...) mc_printf_like(2, 3);

// vm_memory.c
mc_list_s* mc_vm_list(mc_vm_s* const vm, const size_t count);
mc_block_s* mc_vm_block(mc_vm_s* const vm, mc_list_s* const data, mc_object_s* const owner, const mc_block_kind_e kind);
mc_place_s* mc_vm_place(mc_vm_s* const vm, mc_list_s* const container, const int64_t key, mc_object_s* const owner,
						mc_block_s* const block, const bool read_only);
mc_frame_record_s* mc_vm_frame_record(mc_vm_s* const vm, const size_t locals);
void mc_vm_collect(mc_vm_s* const vm);
void mc_vm_free_objects(mc_vm_s* const vm);

/// a deep copy: arrays and structs are values.
mc_value_s mc_vm_copy(mc_vm_s* const vm, const mc_value_s value);

/// the new value of a place: arrays and structs are written in place.
mc_value_s mc_vm_assign(mc_vm_s* const vm, const mc_value_s old, const mc_value_s value);

/// stops when the block's memory was freed or its call returned.
void mc_vm_check(mc_vm_s* const vm, const mc_block_s* const block);
mc_value_s mc_vm_get(mc_vm_s* const vm, const mc_place_s* const place);
void mc_vm_set(mc_vm_s* const vm, const mc_place_s* const place, const mc_value_s value);

/// a slice of 'data' (or of the existing 'block') from 'low' to 'high'.
mc_value_s mc_vm_slice_of(mc_vm_s* const vm, mc_list_s* const data, mc_object_s* const owner, const bool read_only, const int64_t flags,
						  const bool has_low, const __int128 low, const bool has_high, const __int128 high,
						  const int64_t start, const int64_t length, mc_block_s* const block);

/// 'value' in decimal (for messages).
const char* mc_vm_int_text(char buffer[48], const __int128 value);

/// a copy of the zero value of the type descriptor 'type'.
mc_value_s mc_vm_zero(mc_vm_s* const vm, const int64_t type);

// vm_builtins.c
mc_builtin_f mc_vm_find_builtin(const char* const name);
mc_native_f mc_vm_find_native(const char* const name);

/// 'value' as print shows it, by the type descriptor 'type'.
void mc_vm_format(mc_vm_s* const vm, mc_string_s* const out, const mc_value_s value, const int64_t type, const int depth);

/// the bytes of a []u8 slice or a [N]u8 array, as utf-8 text.
void mc_vm_text(mc_vm_s* const vm, mc_string_s* const out, const mc_value_s value);

/// writes what the program prints.
void mc_vm_write(mc_vm_s* const vm, const mc_string_s* const text);

/// a float as print shows it ('bits' 32 for f32).
void mc_vm_format_float(mc_string_s* const out, const double value, const int bits);

static inline mc_value_s mc_make_integer(const __int128 integer) {
	return (mc_value_s) { .kind = mc_value_integer, .as.integer = integer, };
}

static inline mc_value_s mc_make_real(const double real) {
	return (mc_value_s) { .kind = mc_value_real, .as.real = real, };
}

static inline mc_value_s mc_make_boolean(const bool boolean) {
	return (mc_value_s) { .kind = mc_value_boolean, .as.boolean = boolean, };
}

static inline mc_value_s mc_make_list(mc_list_s* const list) {
	return (mc_value_s) { .kind = mc_value_list, .as.list = list, };
}

static inline mc_value_s mc_make_location(mc_place_s* const place) {
	return (mc_value_s) { .kind = mc_value_location, .as.location = place, };
}

static inline mc_value_s mc_make_slice(mc_block_s* const block, const int64_t start, const int64_t length, const bool read_only) {
	mc_value_s value = { .kind = mc_value_slice, };
	value.as.slice.block = block;
	value.as.slice.start = start;
	value.as.slice.length = length;
	value.as.slice.read_only = read_only;
	return value;
}

static inline mc_value_s mc_make_none(void) {
	return (mc_value_s) { .kind = mc_value_none, };
}

#endif
