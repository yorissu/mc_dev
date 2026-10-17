
#ifndef __mc__bytecode_h__
#define __mc__bytecode_h__

/// bytecode: the compiled form of a program, its file format and a
/// disassembler.
///
/// the machine is stack based. every function has numbered local slots
/// (parameters first) and a list of instructions. arithmetic and comparisons
/// are typed by their name, so the machine never inspects values to decide
/// what to do. instructions (operands in brackets, stack effect after ':'):
///
///   CONST [k]                : -> constants[k] (a copy)
///   NONE                     : -> nothing (what a function without a result returns)
///   POP / DUP                : x -> / x -> x x
///   LOAD [slot]              : -> local
///   LOAD_DEREF [slot]        : -> the value of the reference parameter in the slot
///   STORE [slot]             : x -> (writes arrays and structs in place)
///   STORE_NEW [slot]         : x -> (a new variable)
///   LOC_LOCAL [slot]         : -> location of a local
///   LOC_FIELD [i]            : location -> location of field i
///   LOC_INDEX_ARRAY          : location index -> location of the element
///   LOC_INDEX_SLICE          : slice index -> location of the element
///   TEMP_LOC                 : x -> location of a temporary holding x
///   LOAD_LOC / STORE_LOC     : location -> value / location x ->
///   FIELD [i] / SET_FIELD [i]: struct -> field / struct x -> struct
///   MAKE_ARRAY [n]           : x1 .. xn -> array
///   INDEX_ARRAY / INDEX_SLICE: array index -> element / slice index -> element
///   LEN_SLICE                : slice -> uint
///   SLICE_LOC [flags]        : location [low] [high] -> slice of the array at the location
///   SLICE_SLICE [flags]      : slice [low] [high] -> slice
///                              flags: 1 low, 2 high, 4 inclusive, 8 read-only, 16 static
///   SLICE_IS_NULL            : slice -> bool
///   JUMP / JUMP_IF_FALSE / JUMP_IF_TRUE [target]
///   NOT                      : bool -> bool
///   RANGE_INIT [slot, inclusive], RANGE_TEST [slot, target], RANGE_STEP [slot]:
///                              for loops (slots slot..slot+2: current, step, stop)
///   CALL [function, n] / CALL_VALUE [n] / CALL_BUILTIN [builtin, n] / CALL_NATIVE [native, n]
///   FUNCTION [function] / FUNCTION_BUILTIN [builtin] : -> function value
///   RETURN                   : x ->
///   ASSERT_FAIL [k]          : stops with the message constants[k]
///   typed: ADD SUB MUL DIV MOD AND OR XOR SHL SHR EQ NE LT GT LE GE NEG BITNOT, CAST
///
/// the '.mcb' file is the line 'mc-bytecode 1' followed by the module as
/// json; the python implementation reads and writes the same format.

#include "mc/string.h"
#include "mc/vector.h"

typedef enum {
#define mc_opcode(_name, _shape) mc_op_##_name,
#include "mc/opcodes.def"
#undef mc_opcode
	mc_op_count_,
} mc_opcode_e;

/// the type codes of typed instructions.
typedef enum {
	mc_code_s8, mc_code_s16, mc_code_s32, mc_code_s64, mc_code_u8, mc_code_u16, mc_code_u32, mc_code_u64,
	mc_code_int, mc_code_uint, mc_code_f32, mc_code_f64, mc_code_float, mc_code_bool, mc_code_fn,
	mc_type_code_count_,
} mc_type_code_e;

#define mc_slice_low       1
#define mc_slice_high      2
#define mc_slice_inclusive 4
#define mc_slice_read_only 8
#define mc_slice_static    16

typedef struct {
	mc_opcode_e    opcode;
	mc_type_code_e type  ;   // typed instructions; the source type of CAST
	mc_type_code_e target;   // CAST: the target type
	bool           has_a ;
	int64_t        a     ;
	bool           has_b ;
	int64_t        b     ;
	bool           b_bool;   // RANGE_INIT: the pair's second item is a bool
} mc_instruction_s;

typedef struct {
	int32_t  file  ;   // -1: none
	uint32_t line  ;
	uint32_t column;
	uint32_t length;
} mc_position_s;

typedef struct {
	char*                           name      ;
	int64_t                         parameters;
	int64_t                         locals    ;
	bool                            returns   ;
	mc_vector_of(mc_instruction_s)  code      ;
	mc_vector_of(mc_position_s)     positions ;
} mc_bc_function_s;

/// a constant: a tree of values without heap objects.
typedef enum {
	mc_constant_none    ,
	mc_constant_integer ,
	mc_constant_real    ,
	mc_constant_boolean ,
	mc_constant_text    ,   // assert messages
	mc_constant_list    ,   // an array or a struct
	mc_constant_slice   ,
	mc_constant_function,
	mc_constant_builtin ,
} mc_constant_kind_e;

typedef struct mc_bc_constant_s mc_bc_constant_s;

struct mc_bc_constant_s {
	mc_constant_kind_e kind;
	__int128           integer;
	double             real;
	bool               boolean;
	char*              text;
	mc_vector_of(mc_bc_constant_s) items;    // lists, and the data of slices
	int64_t            start;                // slices
	int64_t            length;
	bool               read_only;
	bool               null;                 // a slice without memory
};

/// a type descriptor (for zero values and printing).
typedef struct {
	char*                   name   ;
	char*                   kind   ;   // "int", "float", "bool", "array", "slice", "struct", "function"
	int64_t                 element;
	int64_t                 length ;
	mc_vector_of(char*)     field_names;
	mc_vector_of(int64_t)   field_types;
	int64_t                 zero   ;   // a constant index, -1 for none
} mc_bc_type_s;

typedef struct {
	char*                 name ;
	mc_vector_of(int64_t) types;
} mc_bc_builtin_s;

typedef struct {
	mc_vector_of(mc_bc_function_s) functions    ;
	mc_vector_of(mc_bc_constant_s) constants    ;
	mc_vector_of(mc_bc_type_s)     types        ;
	mc_vector_of(mc_bc_builtin_s)  builtins     ;
	mc_vector_of(char*)            natives      ;
	mc_vector_of(char*)            files        ;
	int64_t                        entry        ;
	bool                           entry_returns;
} mc_module_bc_s;

const char* mc_opcode_name(const mc_opcode_e opcode);

const char* mc_type_code_name(const mc_type_code_e code);

/// the full name of an instruction: 'CONST', 'ADD_S32', 'CAST_INT_F32'.
void mc_instruction_name(const mc_instruction_s* const instruction, mc_string_s* const out);

/// parses an instruction name; false when unknown.
bool mc_instruction_parse(const char* const name, mc_instruction_s* const instruction);

void mc_module_bc_free(mc_module_bc_s* const module);

void mc_bc_constant_free(mc_bc_constant_s* const constant);

/// writes and reads '.mcb' files; false (with 'error' set) when it fails.
bool mc_module_bc_save(const mc_module_bc_s* const module, const char* const path);

bool mc_module_bc_load(const char* const path, mc_module_bc_s* const module, char** const error);

/// a constant as the disassembler shows it.
void mc_bc_constant_text(mc_string_s* const out, const mc_bc_constant_s* const constant);

/// the readable listing of a module.
void mc_disassemble(const mc_module_bc_s* const module, mc_string_s* const out);

#endif
