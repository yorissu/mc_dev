
#ifndef __mc__bytecode_h__
#define __mc__bytecode_h__

/// bytecode: the compiled form of a program, its file format and a
/// disassembler.
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
/// after ':'):
///
///   CONST [k]                   : -> constants[k]
///   ZERO [w]                    : -> w zero words (the zero value of most types)
///   POP [w] / DUP [w]           : x -> / x -> x x
///   LOAD_T [offset]             : -> the local at offset
///   LOAD_BYTES [offset, size]   : -> the struct or array at offset
///   STORE_T [offset]            : x ->
///   STORE_BYTES [offset, size]  : x ->
///   LOC_LOCAL [offset]          : -> location of a local
///   LOC_CONST [k]               : -> read-only location of constant k
///   LOC_FIELD [offset]          : location -> location + offset
///   LOC_INDEX_I [size, length]  : location index -> location of the element (bounds checked)
///   LOC_INDEX_SLICE_I [size]    : slice index -> location of the element (bounds checked)
///   TEMP_LOC_T [offset]         : x -> location of a temporary of the frame holding x
///   TEMP_LOC_BYTES [offset, size]
///   LOAD_LOC_T / LOAD_LOC_BYTES [size]   : location -> value
///   STORE_LOC_T / STORE_LOC_BYTES [size] : location x ->
///   FIELD_T [offset, w]         : struct (w words) -> field
///   FIELD_BYTES [offset, w, size]
///   SET_FIELD_T [offset, w]     : struct (w words) x -> struct
///   SET_FIELD_BYTES [offset, w, size]
///   MAKE_ARRAY_T [n]            : x1 .. xn -> array
///   MAKE_ARRAY_BYTES [n, size]
///   INDEX_T_I [length, w]       : array (w words) index -> element
///   INDEX_BYTES_I [length, w, size]
///   INDEX_SLICE_T_I             : slice index -> element
///   INDEX_SLICE_BYTES_I [size]
///   LEN_SLICE                   : slice -> uint
///   SLICE_IS_NULL               : slice -> bool (whether it is empty)
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
/// the '.mcb' file is binary and little-endian: the magic "MCB\0", the
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
///   intrinsic: name (string), result (s64 type, -1: none), u32 counts of
///              type arguments, of parameters and of variadic values, then
///              each type (u32); flags (u32 count, then u32 each)
///   function:  name (string), parameters (u32), frame (u64 bytes), result
///              words (u32), returns (u8), parameter slots (u32 count, then
///              code (u8), offset (u64), size (u64) each), code (u32 count,
///              then opcode (u8), type (u8), target (u8), operands (s64 each,
///              as many as the opcode takes)), positions (one per
///              instruction: file (s32), line, column, length (u32))

#include "mc/string.h"
#include "mc/vector.h"

typedef enum {
#define mc_opcode(_name, _types, _operands) mc_op_##_name,
#include "mc/opcodes.def"
#undef mc_opcode
	mc_op_count_,
} mc_opcode_e;

/// the type codes of typed instructions: scalars, function values and
/// slices (two words, also used for locations).
typedef enum {
	mc_code_s8, mc_code_s16, mc_code_s32, mc_code_s64, mc_code_u8, mc_code_u16, mc_code_u32, mc_code_u64,
	mc_code_int, mc_code_uint, mc_code_f32, mc_code_f64, mc_code_float, mc_code_bool, mc_code_fn, mc_code_slice,
	mc_type_code_count_,
} mc_type_code_e;

#define mc_slice_low       1
#define mc_slice_high      2
#define mc_slice_inclusive 4
#define mc_slice_read_only 8

/// the format version of '.mcb' files.
#define mc_bytecode_version 2

typedef struct {
	mc_opcode_e    opcode;
	mc_type_code_e type  ;   // the first type code of typed instructions
	mc_type_code_e target;   // the second one (CAST: the target type)
	int64_t        a     ;
	int64_t        b     ;
	int64_t        c     ;
} mc_instruction_s;

typedef struct {
	int32_t  file  ;   // -1: none
	uint32_t line  ;
	uint32_t column;
	uint32_t length;
} mc_position_s;

/// where a parameter lives in the frame.
typedef struct {
	mc_type_code_e code  ;   // mc_type_code_count_: bytes (a struct or an array)
	uint64_t       offset;
	uint64_t       size  ;
} mc_bc_slot_s;

typedef struct {
	char*                           name      ;
	uint32_t                        parameters;
	uint64_t                        frame     ;   // bytes of parameters, locals and temporaries
	uint32_t                        result    ;   // words of the result
	bool                            returns   ;
	mc_vector_of(mc_bc_slot_s)      slots     ;   // of the parameters
	mc_vector_of(mc_instruction_s)  code      ;
	mc_vector_of(mc_position_s)     positions ;
} mc_bc_function_s;

/// a constant: a tree of values without heap memory.
typedef enum {
	mc_constant_none    ,
	mc_constant_integer ,
	mc_constant_real    ,
	mc_constant_boolean ,
	mc_constant_text    ,   // assert messages
	mc_constant_list    ,   // an array or a struct
	mc_constant_slice   ,
	mc_constant_function,
	mc_constant_intrinsic,
} mc_constant_kind_e;

typedef struct mc_bc_constant_s mc_bc_constant_s;

struct mc_bc_constant_s {
	mc_constant_kind_e kind;
	int64_t            type;                 // the type descriptor (-1: a scalar, whose word is its value)
	uint64_t           integer;              // integers (the bits), functions and intrinsics (the index)
	bool               signed_;              // integers: shown as signed
	double             real;
	bool               boolean;
	char*              text;
	mc_vector_of(mc_bc_constant_s) items;    // lists, and the data of slices
	int64_t            start;                // slices
	int64_t            length;
	bool               read_only;
	bool               null;                 // a slice without memory
};

typedef enum {
	mc_descriptor_int, mc_descriptor_float, mc_descriptor_bool, mc_descriptor_array,
	mc_descriptor_slice, mc_descriptor_struct, mc_descriptor_function,
	mc_descriptor_count_,
} mc_descriptor_kind_e;

/// a type descriptor: the layout of a type, for constants, zero values,
/// printing and plugins.
typedef struct {
	char*                   name     ;
	mc_descriptor_kind_e    kind     ;
	mc_type_code_e          code     ;   // scalars, functions and slices; mc_type_code_count_ otherwise
	uint64_t                size     ;
	uint64_t                alignment;
	int64_t                 element  ;   // arrays and slices
	int64_t                 length   ;   // arrays
	mc_vector_of(char*)     field_names;
	mc_vector_of(int64_t)   field_types;
	mc_vector_of(uint64_t)  field_offsets;
	int64_t                 zero     ;   // a constant when the zero value is not all zero bytes; -1 otherwise
} mc_bc_type_s;

typedef mc_vector_of(int64_t) mc_bc_indexes_s;

/// a use of a function that the machine does not run itself (a plugin
/// function): its qualified name and the types of one use.
typedef struct {
	char*                 name     ;   // 'memory.alloc'
	mc_bc_indexes_s       arguments;   // type arguments
	mc_bc_indexes_s       parameters;  // the types of the fixed parameters
	mc_bc_indexes_s       variadic ;   // the types of the values given for '...'
	int64_t               result   ;   // the type of the result, -1 for none
	mc_vector_of(uint32_t) flags   ;   // per argument, set by the plugin's check
} mc_bc_intrinsic_s;

typedef struct {
	mc_vector_of(mc_bc_function_s)  functions    ;
	mc_vector_of(mc_bc_constant_s)  constants    ;
	mc_vector_of(mc_bc_type_s)      types        ;
	mc_vector_of(mc_bc_intrinsic_s) intrinsics   ;
	mc_vector_of(char*)             files        ;
	int64_t                         entry        ;
	bool                            entry_returns;
} mc_module_bc_s;

const char* mc_opcode_name(const mc_opcode_e opcode);

/// how many type codes and integer operands an instruction takes.
int mc_opcode_types(const mc_opcode_e opcode);

int mc_opcode_operands(const mc_opcode_e opcode);

const char* mc_type_code_name(const mc_type_code_e code);

/// the full name of an instruction: 'CONST', 'ADD_S32', 'CAST_INT_F32'.
void mc_instruction_name(const mc_instruction_s* const instruction, mc_string_s* const out);

/// parses an instruction name; false when unknown.
bool mc_instruction_parse(const char* const name, mc_instruction_s* const instruction);

/// the bytes of a value of a type code (8 for words, 16 for slices).
uint64_t mc_type_code_size(const mc_type_code_e code);

void mc_module_bc_free(mc_module_bc_s* const module);

void mc_bc_constant_free(mc_bc_constant_s* const constant);

/// writes and reads '.mcb' files; false (with 'error' set, caller frees) when it fails.
bool mc_module_bc_save(const mc_module_bc_s* const module, const char* const path);

bool mc_module_bc_load(const char* const path, mc_module_bc_s* const module, char** const error);

/// the same, in memory.
void mc_module_bc_write(const mc_module_bc_s* const module, mc_string_s* const out);

bool mc_module_bc_read(const mc_view_s data, mc_module_bc_s* const module, char** const error);

/// a constant as the disassembler shows it.
void mc_bc_constant_text(mc_string_s* const out, const mc_bc_constant_s* const constant);

/// the readable listing of a module.
void mc_disassemble(const mc_module_bc_s* const module, mc_string_s* const out);

#endif
