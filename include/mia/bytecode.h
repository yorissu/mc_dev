
#ifndef __mia__bytecode_h__
#define __mia__bytecode_h__

/// bytecode: the compiled form of a program and its file format.
///
/// values. the machine stores data the way c does: scalars at their real
/// size (u8 1 byte, s32 4, int, uint and float 8, bool 1), arrays as
/// contiguous elements and structs as fields in declaration order with c
/// alignment and padding; a slice is 16 bytes (a block handle, a byte offset
/// and a length), a function value 8. the operand stack holds 64-bit words:
/// a scalar is one word (integers extended by their type, f32 widened to a
/// double, bools 0 or 1), a slice or a location two (handle and offset), and
/// a struct or an array its bytes, rounded up to whole words.
///
/// every function has a frame: a byte area for its parameters, locals and
/// temporaries, laid out by the compiler (offsets in bytes). a location is
/// a block handle and a byte offset: a local of a running call, a part of a
/// heap allocation or of constant data. every access through a location or
/// a slice checks that its memory is still alive.
///
/// instructions (operands in brackets; T is a type code, I the type code of
/// an index; 'w' counts words, sizes and offsets are bytes; the stack effect
/// after ':'). T is BYTES for a struct or an array; an instruction that
/// moves a value of type T has its size as its last operand ('size', which
/// for the other codes is the size of the code: 1 for U8, 16 for SLICE):
///
///   CONST [k]                   : -> constants[k]
///   ZERO [w]                    : -> w zero words (the zero value of most types)
///   POP [w] / DUP [w]           : x -> / x -> x x
///   LOAD_T [offset, size]       : -> the local at offset
///   STORE_T [offset, size]      : x ->
///   LOC_LOCAL [offset]          : -> location of a local
///   LOC_CONST [k]               : -> read-only location of constant k
///   LOC_FIELD [offset]          : location -> location + offset
///   LOC_INDEX_I [size, length]  : location index -> location of the element (bounds checked)
///   LOC_INDEX_SLICE_I [size]    : slice index -> location of the element (bounds checked)
///   TEMP_LOC_T [offset, size]   : x -> location of a temporary of the frame holding x
///   LOAD_LOC_T [size]           : location -> value
///   STORE_LOC_T [size]          : location x ->
///   FIELD_T [offset, w, size]   : struct (w words) -> field
///   SET_FIELD_T [offset, w, size] : struct (w words) x -> struct
///   MAKE_ARRAY_T [n, size]      : x1 .. xn -> array
///   INDEX_T_I [length, w, size] : array (w words) index -> element
///   INDEX_SLICE_T_I [size]      : slice index -> element
///   LEN_SLICE                   : slice -> uint
///   SLICE_IS_NULL               : slice -> bool (whether it is empty; no longer emitted, kept so older .miab files load)
///   SLICE_LOC_L_H [flags, length, size] : location [low] [high] -> slice of the array at the location
///   SLICE_SLICE_L_H [flags, size]       : slice [low] [high] -> slice
///                               flags: 1 low, 2 high, 4 inclusive, 8 read-only;
///                               L and H are the types of low and high
///   JUMP / JUMP_IF_FALSE / JUMP_IF_TRUE [target]
///   NOT                         : bool -> bool
///   RANGE_INIT_T [offset, inclusive] : start end -> (the range state at offset: current, step, stop)
///   RANGE_TEST [offset, target] : jumps to target when the range is done
///   RANGE_STEP [offset]
///   CALL [function]             : arguments -> result
///   CALL_VALUE [w, r]           : function arguments (w words) -> result (r words)
///   CALL_INTRINSIC [intrinsic, w] : arguments (w words) -> result
///   FUNCTION [function] / INTRINSIC [intrinsic] : -> function value
///   RETURN [w]                  : result (w words) ->
///   ASSERT_FAIL [k]             : stops with the message constants[k]
///   ADD SUB MUL DIV MOD AND OR XOR EQ NE LT GT LE GE NEG BITNOT _T,
///   SHL_T_U SHR_T_U (U: the type of the count), CAST_FROM_TO
///
/// the '.miab' file is binary and little-endian: the magic "MIA\0", the
/// format version (u32), then the sections in this order, each a u32 count
/// of entries: files (strings), types, constants, intrinsics, functions,
/// and finally the entry function (u32) and whether it returns a value
/// (u8). integers are fixed width: u8, u32, u64 or s64; a string is its
/// length (u32) and its bytes. the reader checks every count and index, so
/// a truncated or corrupted file is refused with a message.
///
///   type:      name (string), kind (u8), code (u8), size (u64), alignment (u64),
///              element (u32), length (s64), zero (s64: a constant or -1),
///              fields (u32 count, then name (string), type (u32), offset (u64) each)
///   constant:  kind (u8), type (s64), then by kind: integer (u64 bits, u8 signed),
///              real (u64 bits), boolean (u8), text (string), function (u64),
///              list (u32 count, constants), slice (u8 null, u8 read-only,
///              s64 start, s64 length, u32 count, constants)
///   intrinsic: name (string), plugin (string), result (s64 type, -1: none), u32 counts of
///              type arguments, of parameters and of variadic values, then
///              each type (u32); flags (u32 count, then u32 each)
///   function:  name (string), parameters (u32), frame (u64 bytes), result
///              words (u32), returns (u8), parameter slots (u32 count, then
///              code (u8), offset (u64), size (u64) each), code (u32 count,
///              then opcode (u8), type (u8), target (u8), operands (s64 each,
///              as many as the opcode takes)), positions (one per
///              instruction: file (s32), line, column, length (u32))

#include "mia/code.h"
#include "mia/opcode.h"
#include "mia/string.h"
#include "mia/vector.h"

#define mia_bytecode_slice_low       1
#define mia_bytecode_slice_high      2
#define mia_bytecode_slice_inclusive 4
#define mia_bytecode_slice_read_only 8

/// the largest a type may be, and a function's frame (in bytes): the
/// checker reports larger types (E0532), the compiler larger frames, and
/// the machine refuses a module that has them.
#define mia_bytecode_type_size_limit  ((uint64_t)1 << 32)
#define mia_bytecode_frame_size_limit ((uint64_t)1 << 31)

/// how deep constants may nest (arrays and structs in each other): as deep
/// as types can (mia_ast_depth_limit).
#define mia_bytecode_nesting_limit 1000

/// the format version of '.miab' files.
#define mia_bytecode_version 4

typedef struct {
	mia_opcode_e opcode;
	mia_code_e   type  ;   // the first type code of typed instructions
	mia_code_e   target;   // the second one (CAST: the target type)
	int64_t      a     ;
	int64_t      b     ;
	int64_t      c     ;
} mia_bytecode_instruction_s;

typedef struct {
	int32_t  file  ;   // -1: none
	uint32_t line  ;
	uint32_t column;
	uint32_t length;
} mia_bytecode_position_s;

/// where a parameter lives in the frame.
typedef struct {
	mia_code_e code  ;
	uint64_t   offset;
	uint64_t   size  ;
} mia_bytecode_slot_s;

typedef struct {
	char*                                     name      ;
	uint32_t                                  parameters;
	uint64_t                                  frame     ;   // bytes of parameters, locals and temporaries
	uint32_t                                  result    ;   // words of the result
	bool                                      returns   ;
	mia_vector_of(mia_bytecode_slot_s)        slots     ;   // of the parameters
	mia_vector_of(mia_bytecode_instruction_s) code      ;
	mia_vector_of(mia_bytecode_position_s)    positions ;
} mia_bytecode_function_s;

/// a constant: a tree of values without heap memory.
typedef enum {
	mia_bytecode_constant_none    ,
	mia_bytecode_constant_integer ,
	mia_bytecode_constant_real    ,
	mia_bytecode_constant_boolean ,
	mia_bytecode_constant_text    ,   // assert messages
	mia_bytecode_constant_list    ,   // an array or a struct
	mia_bytecode_constant_slice   ,
	mia_bytecode_constant_function,
	mia_bytecode_constant_intrinsic,
} mia_bytecode_constant_kind_e;

typedef struct mia_bytecode_constant_s mia_bytecode_constant_s;

struct mia_bytecode_constant_s {
	mia_bytecode_constant_kind_e           kind;
	int64_t                                type;        // the type descriptor (-1: a scalar, whose word is its value)
	uint64_t                               integer;     // integers (the bits), functions and intrinsics (the index)
	bool                                   is_signed;   // integers: shown as signed
	double                                 real;
	bool                                   boolean;
	char*                                  text;
	mia_vector_of(mia_bytecode_constant_s) items;       // lists, and the data of slices
	int64_t                                start;       // slices
	int64_t                                length;
	bool                                   read_only;
	bool                                   null;        // a slice without memory
};

typedef enum {
	mia_bytecode_descriptor_int, mia_bytecode_descriptor_float, mia_bytecode_descriptor_bool, mia_bytecode_descriptor_array,
	mia_bytecode_descriptor_slice, mia_bytecode_descriptor_struct, mia_bytecode_descriptor_function,
	mia_bytecode_descriptor_count,
} mia_bytecode_descriptor_kind_e;

/// a type descriptor: the layout of a type, for constants, zero values,
/// printing and plugins.
typedef struct {
	char*                          name         ;
	mia_bytecode_descriptor_kind_e kind         ;
	mia_code_e                     code         ;   // BYTES for structs and arrays
	uint64_t                       size         ;
	uint64_t                       alignment    ;
	int64_t                        element      ;   // arrays and slices
	int64_t                        length       ;   // arrays
	mia_vector_of(char*)           field_names  ;
	mia_vector_of(int64_t)         field_types  ;
	mia_vector_of(uint64_t)        field_offsets;
	int64_t                        zero         ;   // a constant when the zero value is not all zero bytes; -1 otherwise
} mia_bytecode_type_s;

typedef mia_vector_of(int64_t) mia_bytecode_indexes_s;

/// a use of a function that the machine does not run itself (a plugin
/// function): its qualified name and the types of one use.
typedef struct {
	char*                   name      ;   // 'memory.alloc'
	char*                   plugin    ;   // the plugin that provides it: 'memory'
	mia_bytecode_indexes_s  arguments ;   // type arguments
	mia_bytecode_indexes_s  parameters;   // the types of the fixed parameters
	mia_bytecode_indexes_s  variadic  ;   // the types of the values given for '...'
	int64_t                 result    ;   // the type of the result, -1 for none
	mia_vector_of(uint32_t) flags     ;   // per argument, set by the plugin's check
} mia_bytecode_intrinsic_s;

typedef struct {
	mia_vector_of(mia_bytecode_function_s)  functions    ;
	mia_vector_of(mia_bytecode_constant_s)  constants    ;
	mia_vector_of(mia_bytecode_type_s)      types        ;
	mia_vector_of(mia_bytecode_intrinsic_s) intrinsics   ;
	mia_vector_of(char*)                    files        ;
	int64_t                                 entry        ;
	bool                                    entry_returns;
} mia_bytecode_module_s;

/// frees what a module owns (the module itself is the caller's).
void mia_bytecode_free(mia_bytecode_module_s* const module);

void mia_bytecode_constant_free(mia_bytecode_constant_s* const constant);

/// writes and reads '.miab' files; false (with 'error' set, caller frees) when it fails.
bool mia_bytecode_save(const mia_bytecode_module_s* const module, const char* const path);

bool mia_bytecode_load(const char* const path, mia_bytecode_module_s* const module, char** const error);

/// the same, in memory.
void mia_bytecode_write(const mia_bytecode_module_s* const module, mia_string_s* const out);

bool mia_bytecode_read(const mia_view_s data, mia_bytecode_module_s* const module, char** const error);

#endif
