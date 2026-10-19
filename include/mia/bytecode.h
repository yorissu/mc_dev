
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
///   RANGE_NEXT [offset, target] : steps the range, then jumps to target unless it is done
///   RANGE_STEP [offset]         : steps the range (no longer emitted, kept so older .miab files load)
///   CALL [function]             : arguments -> result
///   CALL_VALUE [w, r]           : function arguments (w words) -> result (r words)
///   CALL_INTRINSIC [intrinsic, w] : arguments (w words) -> result
///   FUNCTION [function] / INTRINSIC [intrinsic] : -> function value
///   RETURN [w]                  : result (w words) ->
///   ASSERT_FAIL [k]             : stops with the message constants[k]
///   ADD SUB MUL DIV MOD AND OR XOR EQ NE LT GT LE GE NEG BITNOT _T,
///   SHL_T_U SHR_T_U (U: the type of the count), CAST_FROM_TO
///
/// the '.miab' file is binary: the magic "MIA\0", the format version (4
/// bytes, little-endian), then the sections in this order, each a count of
/// entries: files (strings), types, constants, intrinsics, functions, and
/// finally the entry function (u) and whether it returns a value (u8).
/// numbers are varints: u is unsigned (7 bits a byte, the lowest first, the
/// top bit set when another byte follows; at most 10 bytes), s is signed
/// (zigzag: 0, -1, 1, -2... written as the u 0, 1, 2, 3...); a u8 is one
/// byte, an f64 its 8 bytes little-endian; a string is its length (u) and its
/// bytes. counts and indexes fit in 32 bits. the reader checks every number,
/// count and index, so a truncated or corrupted file is refused with a message.
///
///   type:      name (string), kind (u8), code (u8), size (u), alignment (u),
///              element (u), length (s), zero (s: a constant or -1),
///              fields (u count, then name (string), type (u), offset (u) each)
///   constant:  kind (u8), type (s), then by kind: integer (u8 signed, then the
///              value: s when signed, else u), real (f64), boolean (u8), text
///              (string), function and intrinsic (u), list (u count, constants),
///              slice (u8 null, u8 read-only, s start, s length, u count, constants)
///   intrinsic: name (string), plugin (string), result (s type, -1: none), u counts of
///              type arguments, of parameters and of variadic values, then
///              each type (u); flags (u count, then u each)
///   function:  name (string), parameters (u), frame (u bytes), result
///              words (u), returns (u8), parameter slots (u count, then
///              code (u8), offset (u), size (u) each), code (u count, then
///              opcode (u8), its type codes (u8 each, as many as the opcode
///              has), operands (s each, as many as it takes)), then a position
///              per instruction, as what changed from the one before (the
///              first from file -1, line, column and length 0): a u8 of the
///              parts that changed (1 file, 2 line, 4 column, 8 length), then
///              those: file, line and column as s differences, length as u

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
#define mia_bytecode_version 5

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
