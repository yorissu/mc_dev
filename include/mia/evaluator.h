
#ifndef __mia__evaluator_h__
#define __mia__evaluator_h__

/// the compile-time evaluator's values and state (see evaluator.c).

#include "mia/program.h"

#include <setjmp.h>

typedef struct mia_checker_s mia_checker_s;

typedef enum {
	mia_evaluator_value_nothing ,
	mia_evaluator_value_integer ,
	mia_evaluator_value_real    ,
	mia_evaluator_value_boolean ,
	mia_evaluator_value_list    ,   // an array or a struct (fields in declaration order)
	mia_evaluator_value_slice   ,
	mia_evaluator_value_function,
	mia_evaluator_value_cell    ,   // a reference argument: the place it refers to
} mia_evaluator_value_kind_e;

typedef struct mia_evaluator_list_s mia_evaluator_list_s;

struct mia_evaluator_value_s {
	mia_evaluator_value_kind_e kind;
	union {
		mia_int128_s          integer;
		double                real;
		bool                  boolean;
		mia_evaluator_list_s* list;
		struct { mia_evaluator_list_s* list; int64_t start; int64_t length; } slice;
		struct { mia_program_instance_s* instance; mia_ast_node_s* lambda; } function;
		mia_evaluator_value_s* cell;
	} as;
};

struct mia_evaluator_list_s {
	size_t                count;
	mia_evaluator_value_s items[];
};

typedef enum {
	mia_evaluator_ok          ,
	mia_evaluator_not_constant,   // 'node' and 'reason' say why
	mia_evaluator_failed      ,   // an error was reported already (unless quiet)
} mia_evaluator_status_e;

typedef struct {
	mia_evaluator_status_e status;
	mia_evaluator_value_s  value ;
	const mia_ast_node_s*  node  ;
	char*                  reason;   // owned by the program arena
} mia_evaluator_result_s;

typedef struct mia_evaluator_frame_s mia_evaluator_frame_s;

/// a loop or a call that is running in an evaluation (for E0705).
typedef struct {
	const mia_ast_node_s* node    ;   // the while, the for or the call
	const char*           function;   // calls: the function called; loops: null
} mia_evaluator_running_s;

/// the values in lists (array elements, struct fields) one evaluation may
/// make: each takes memory at compile time and in the bytecode.
#define mia_evaluator_cell_limit ((uint64_t)1 << 22)

typedef struct {
	mia_checker_s*                         checker;
	mia_vector_of(mia_evaluator_frame_s*)  frames ;
	mia_vector_of(mia_evaluator_running_s) running;   // innermost last
	uint64_t                               steps  ;
	uint64_t                               limit  ;   // of steps
	uint64_t                               cells  ;   // values made in lists by this evaluation
	bool                                   quiet  ;
	bool                                   active ;
	jmp_buf*                               stop   ;
	mia_evaluator_result_s                 problem;
} mia_evaluator_s;

/// the compile-time value of the checked expression 'node'. quiet evaluation
/// runs no functions and reports nothing; it only probes for a known value.
mia_evaluator_result_s mia_evaluator_evaluate(mia_evaluator_s* const evaluator, const mia_ast_node_s* const node, const bool quiet);

/// a deep copy (arrays and structs are values).
mia_evaluator_value_s mia_evaluator_value_copy(mia_checker_s* const checker, const mia_evaluator_value_s value);

/// the result of an arithmetic or bit operator on 128-bit integers; false on
/// division by zero. 'overflow' says when the exact result needs more than
/// 128 bits ('result' then has its low 128 bits).
bool mia_evaluator_integer_arithmetic(const mia_ast_operator_e operation, const mia_int128_s left, const mia_int128_s right, mia_int128_s* const result,
									  bool* const overflow);

/// 'value' wrapped around to fit the integer scalar 'type'.
mia_int128_s mia_evaluator_integer_wrap(const mia_int128_s value, const mia_type_s* const type);

/// 'value' rounded as the float scalar 'type' (f32 rounds).
double mia_evaluator_float_wrap(const double value, const mia_type_s* const type);

#endif
