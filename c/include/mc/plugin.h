
#ifndef __mc__plugin_h__
#define __mc__plugin_h__

/// the plugin api: how c code provides functions to mc programs.
///
/// a plugin is a shared library (or code compiled into mc, like the 'std'
/// plugin) with one entry point, 'mc_plugin_init', which registers the
/// functions it provides under their qualified names: the dotted name of
/// the mc module that declares them and the function's name.
///
///     # math.mc, in a module directory of the program
///     [plugin = "mymath"]
///     pub sqrt :: (x: f64) -> f64;
///
///     // mymath.c, built as a shared library
///     #include "mc/plugin.h"
///     #include <math.h>
///
///     static mc_plugin_value_s _sqrt(mc_plugin_call_s* const call) {
///         return mc_plugin_make_f64(call, sqrt(mc_plugin_get_f64(call, 0)));
///     }
///
///     MC_PLUGIN_EXPORT bool mc_plugin_init(mc_plugin_api_s* const api) {
///         if (!mc_plugin_compatible(api, MC_PLUGIN_API_VERSION)) return false;
///         return mc_plugin_register(api, "mymath", "math.sqrt", _sqrt, NULL);
///     }
///
///     mc run --plugin ./mymath.so main.mc
///
/// the checker types every call from the mc declaration, so a function can
/// trust the number and the types of its arguments. a function may also
/// have a check: it runs at compile time for every call, sees the static
/// types of the arguments (and their values when the compiler knows them)
/// and can report errors and warnings like the compiler does.
///
/// everything goes through the functions below and opaque handles: a
/// plugin never sees the machine's own structures, so they can change
/// without breaking plugins. the functions come from a table that the
/// handles carry ('interface'); the table only grows, and its version says
/// what it has. data is laid out like c lays it out: an argument's memory
/// ('mc_plugin_get_memory') and the elements of a slice are the bytes a c
/// struct or array of the same fields would have (sizes, offsets and
/// alignments come from the type functions). slices and function values
/// inside such memory are read with 'mc_plugin_slice_at' and
/// 'mc_plugin_function_name_at'.

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// the version of this api. a plugin passes the version it was built with
/// to mc_plugin_compatible; mc refuses plugins that need a newer one.
#define MC_PLUGIN_API_VERSION 1

#if defined(_WIN32)
#	define MC_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#	define MC_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#	define MC_PLUGIN_EXPORT
#endif

#if defined(__GNUC__) && !defined(MC_PORTABLE)
#	define MC_PLUGIN_PRINTF(_format, _arguments) __attribute__((format(printf, _format, _arguments)))
#else
#	define MC_PLUGIN_PRINTF(_format, _arguments)
#endif

#if defined(_MSC_VER)
#	define MC_PLUGIN_NORETURN __declspec(noreturn)
#else
#	define MC_PLUGIN_NORETURN _Noreturn
#endif

typedef struct mc_plugin_interface_s mc_plugin_interface_s;

/// the handle of the registration (given to mc_plugin_init).
typedef struct {
	const mc_plugin_interface_s* interface;
} mc_plugin_api_s;

/// the handle of one call at run time.
typedef struct {
	const mc_plugin_interface_s* interface;
} mc_plugin_call_s;

/// the handle of one call while it is checked at compile time.
typedef struct {
	const mc_plugin_interface_s* interface;
} mc_plugin_check_s;

/// a type (opaque; ask the type functions).
typedef struct mc_plugin_type_s mc_plugin_type_s;

/// a diagnostic being reported by a check (opaque).
typedef struct mc_plugin_diagnostic_s mc_plugin_diagnostic_s;

/// the result of a call: made by the mc_plugin_make_* functions (opaque).
typedef struct {
	uint32_t kind    ;
	uint64_t words[2];
} mc_plugin_value_s;

typedef enum {
	mc_plugin_kind_int     ,   // s8 .. u64, int, uint
	mc_plugin_kind_float   ,   // f32, f64, float
	mc_plugin_kind_bool    ,
	mc_plugin_kind_array   ,
	mc_plugin_kind_slice   ,
	mc_plugin_kind_struct  ,
	mc_plugin_kind_function,
} mc_plugin_kind_e;

/// a slice argument: its elements (null when it is empty), how many, their
/// type, and whether it may be written through.
typedef struct {
	void*                   data     ;
	uint64_t                length   ;
	const mc_plugin_type_s* element  ;
	bool                    read_only;
} mc_plugin_slice_s;

/// what a slice argument views, for functions that manage heap memory.
typedef enum {
	mc_plugin_heap_null ,   // the null (empty) slice
	mc_plugin_heap_whole,   // a whole allocation (of mc_plugin_heap_alloc)
	mc_plugin_heap_part ,   // a part of an allocation
	mc_plugin_heap_other,   // memory that is not heap memory
} mc_plugin_heap_e;

/// the value of an argument that the compiler knows.
typedef enum {
	mc_plugin_constant_none   ,   // not known at compile time
	mc_plugin_constant_integer,
	mc_plugin_constant_float  ,
	mc_plugin_constant_bool   ,
	mc_plugin_constant_bytes  ,   // a []u8 or [N]u8
	mc_plugin_constant_other  ,   // known, but none of the above
} mc_plugin_constant_kind_e;

typedef struct {
	mc_plugin_constant_kind_e kind    ;
	int64_t                   integer ;   // the bits of the value (unsigned types: read as uint64_t)
	double                    real    ;
	bool                      boolean ;
	const uint8_t*            bytes   ;   // valid during the check
	size_t                    length  ;
	bool                      c_string;   // the argument comes from a "..."c literal (also when its value is not known)
} mc_plugin_constant_s;

/// the run function of a plugin function, and its optional check.
typedef mc_plugin_value_s (*mc_plugin_run_f)(mc_plugin_call_s* const call);
typedef void (*mc_plugin_check_f)(mc_plugin_check_s* const check);

/// the argument index that means the whole call (in reports and labels).
#define MC_PLUGIN_CALL ((size_t)-1)

struct mc_plugin_interface_s {
	uint32_t version;
	uint32_t size   ;   // of this table, in bytes
	// registration
	bool (*compatible)(mc_plugin_api_s* const api, const uint32_t version);
	bool (*register_function)(mc_plugin_api_s* const api, const char* const plugin, const char* const name, mc_plugin_run_f run,
							  mc_plugin_check_f check);
	// types
	mc_plugin_kind_e (*type_kind)(const mc_plugin_type_s* const type);
	const char* (*type_name)(const mc_plugin_type_s* const type);
	uint64_t (*type_size)(const mc_plugin_type_s* const type);
	uint64_t (*type_alignment)(const mc_plugin_type_s* const type);
	uint32_t (*type_bits)(const mc_plugin_type_s* const type);
	bool (*type_signed)(const mc_plugin_type_s* const type);
	const mc_plugin_type_s* (*type_element)(const mc_plugin_type_s* const type);
	uint64_t (*type_length)(const mc_plugin_type_s* const type);
	size_t (*type_field_count)(const mc_plugin_type_s* const type);
	const char* (*type_field_name)(const mc_plugin_type_s* const type, const size_t index);
	const mc_plugin_type_s* (*type_field_type)(const mc_plugin_type_s* const type, const size_t index);
	uint64_t (*type_field_offset)(const mc_plugin_type_s* const type, const size_t index);
	// a call at run time
	size_t (*count)(mc_plugin_call_s* const call);
	size_t (*fixed_count)(mc_plugin_call_s* const call);
	const mc_plugin_type_s* (*argument_type)(mc_plugin_call_s* const call, const size_t index);
	size_t (*type_argument_count)(mc_plugin_call_s* const call);
	const mc_plugin_type_s* (*type_argument)(mc_plugin_call_s* const call, const size_t index);
	uint32_t (*argument_flags)(mc_plugin_call_s* const call, const size_t index);
	int64_t (*get_int)(mc_plugin_call_s* const call, const size_t index);
	uint64_t (*get_uint)(mc_plugin_call_s* const call, const size_t index);
	double (*get_float)(mc_plugin_call_s* const call, const size_t index);
	bool (*get_bool)(mc_plugin_call_s* const call, const size_t index);
	const void* (*get_memory)(mc_plugin_call_s* const call, const size_t index);
	mc_plugin_slice_s (*get_slice)(mc_plugin_call_s* const call, const size_t index);
	uint64_t (*slice_length)(mc_plugin_call_s* const call, const size_t index);
	bool (*slice_read_only)(mc_plugin_call_s* const call, const size_t index);
	mc_plugin_slice_s (*slice_at)(mc_plugin_call_s* const call, const void* const memory, const mc_plugin_type_s* const type);
	const char* (*function_name_at)(mc_plugin_call_s* const call, const void* const memory);
	mc_plugin_value_s (*make_int)(mc_plugin_call_s* const call, const int64_t value);
	mc_plugin_value_s (*make_uint)(mc_plugin_call_s* const call, const uint64_t value);
	mc_plugin_value_s (*make_float)(mc_plugin_call_s* const call, const double value);
	mc_plugin_value_s (*make_bool)(mc_plugin_call_s* const call, const bool value);
	mc_plugin_value_s (*make_none)(mc_plugin_call_s* const call);
	mc_plugin_value_s (*make_null)(mc_plugin_call_s* const call);
	mc_plugin_value_s (*heap_alloc)(mc_plugin_call_s* const call, const mc_plugin_type_s* const element, const uint64_t count, void** const data);
	mc_plugin_value_s (*heap_adopt)(mc_plugin_call_s* const call, const mc_plugin_type_s* const element, const uint64_t count, void* const memory,
									void (*release)(void* const memory));
	mc_plugin_heap_e (*heap_of)(mc_plugin_call_s* const call, const size_t index);
	void (*heap_free)(mc_plugin_call_s* const call, const size_t index);
	void (*write)(mc_plugin_call_s* const call, const char* const data, const size_t length);
	void (*fail)(mc_plugin_call_s* const call, const char* const format, va_list args);
	// a call at compile time
	const char* (*check_name)(mc_plugin_check_s* const check);
	size_t (*check_count)(mc_plugin_check_s* const check);
	size_t (*check_fixed_count)(mc_plugin_check_s* const check);
	const mc_plugin_type_s* (*check_type)(mc_plugin_check_s* const check, const size_t index);
	size_t (*check_type_argument_count)(mc_plugin_check_s* const check);
	const mc_plugin_type_s* (*check_type_argument)(mc_plugin_check_s* const check, const size_t index);
	mc_plugin_constant_s (*check_constant)(mc_plugin_check_s* const check, const size_t index);
	void (*check_mark)(mc_plugin_check_s* const check, const size_t index, const uint32_t flags);
	mc_plugin_diagnostic_s* (*report)(mc_plugin_check_s* const check, const bool warning, const char* const code, const size_t index,
									  const char* const message);
	void (*label)(mc_plugin_diagnostic_s* const diagnostic, const size_t index, const char* const text);
	void (*note)(mc_plugin_diagnostic_s* const diagnostic, const char* const text);
	void (*help)(mc_plugin_diagnostic_s* const diagnostic, const char* const text);
};

// --- registration -------------------------------------------------------------------------

/// whether this mc can run a plugin built with api 'version'; when it
/// cannot, mc_plugin_init should return false (mc explains why).
static inline bool mc_plugin_compatible(mc_plugin_api_s* const api, const uint32_t version) {
	return api->interface->compatible(api, version);
}

/// provides the function 'name' ('module.function') as part of 'plugin';
/// 'check' may be null. false when the name is provided already.
static inline bool mc_plugin_register(mc_plugin_api_s* const api, const char* const plugin, const char* const name, mc_plugin_run_f run,
									  mc_plugin_check_f check) {
	return api->interface->register_function(api, plugin, name, run, check);
}

// --- types (at run time and at compile time) ----------------------------------------------

#define _mc_plugin_type_call(_handle, _function, ...) ((_handle)->interface->_function(__VA_ARGS__))

/// the types of arguments come from mc_plugin_argument_type or
/// mc_plugin_check_type; these functions need the call or check handle
/// only to reach the function table.
#define mc_plugin_type_kind(_handle, _type)            _mc_plugin_type_call(_handle, type_kind, _type)
#define mc_plugin_type_name(_handle, _type)            _mc_plugin_type_call(_handle, type_name, _type)
#define mc_plugin_type_size(_handle, _type)            _mc_plugin_type_call(_handle, type_size, _type)
#define mc_plugin_type_alignment(_handle, _type)       _mc_plugin_type_call(_handle, type_alignment, _type)
#define mc_plugin_type_bits(_handle, _type)            _mc_plugin_type_call(_handle, type_bits, _type)
#define mc_plugin_type_signed(_handle, _type)          _mc_plugin_type_call(_handle, type_signed, _type)
#define mc_plugin_type_element(_handle, _type)         _mc_plugin_type_call(_handle, type_element, _type)
#define mc_plugin_type_length(_handle, _type)          _mc_plugin_type_call(_handle, type_length, _type)
#define mc_plugin_type_field_count(_handle, _type)     _mc_plugin_type_call(_handle, type_field_count, _type)
#define mc_plugin_type_field_name(_handle, _type, _i)  _mc_plugin_type_call(_handle, type_field_name, _type, _i)
#define mc_plugin_type_field_type(_handle, _type, _i)  _mc_plugin_type_call(_handle, type_field_type, _type, _i)
#define mc_plugin_type_field_offset(_handle, _type, _i) _mc_plugin_type_call(_handle, type_field_offset, _type, _i)

// --- a call at run time ---------------------------------------------------------------------

/// the number of arguments (the values given for '...' included), and of
/// the fixed ones (the variadic values follow them).
static inline size_t mc_plugin_count(mc_plugin_call_s* const call) {
	return call->interface->count(call);
}

static inline size_t mc_plugin_fixed_count(mc_plugin_call_s* const call) {
	return call->interface->fixed_count(call);
}

static inline const mc_plugin_type_s* mc_plugin_argument_type(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->argument_type(call, index);
}

/// the type arguments of a generic function ('T' of 'alloc<T>').
static inline size_t mc_plugin_type_argument_count(mc_plugin_call_s* const call) {
	return call->interface->type_argument_count(call);
}

static inline const mc_plugin_type_s* mc_plugin_type_argument(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->type_argument(call, index);
}

/// what the check marked on an argument at compile time (0 when nothing).
static inline uint32_t mc_plugin_argument_flags(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->argument_flags(call, index);
}

/// an integer, float or bool argument (any width; f32 is widened).
static inline int64_t mc_plugin_get_int(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->get_int(call, index);
}

static inline uint64_t mc_plugin_get_uint(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->get_uint(call, index);
}

static inline double mc_plugin_get_f64(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->get_float(call, index);
}

static inline bool mc_plugin_get_bool(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->get_bool(call, index);
}

/// the bytes of any argument, laid out like c (a struct, an array, a scalar).
static inline const void* mc_plugin_get_memory(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->get_memory(call, index);
}

/// a slice argument (stops the program when its memory is no longer alive).
static inline mc_plugin_slice_s mc_plugin_get_slice(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->get_slice(call, index);
}

/// the length and the read-only flag of a slice argument, without touching its memory.
static inline uint64_t mc_plugin_slice_length(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->slice_length(call, index);
}

static inline bool mc_plugin_slice_read_only(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->slice_read_only(call, index);
}

/// a slice of 'type' stored in memory (a field, an element).
static inline mc_plugin_slice_s mc_plugin_slice_at(mc_plugin_call_s* const call, const void* const memory, const mc_plugin_type_s* const type) {
	return call->interface->slice_at(call, memory, type);
}

/// the name of the function value stored in memory.
static inline const char* mc_plugin_function_name_at(mc_plugin_call_s* const call, const void* const memory) {
	return call->interface->function_name_at(call, memory);
}

/// results: converted to the declared result type (integers wrap, f32 rounds).
static inline mc_plugin_value_s mc_plugin_make_int(mc_plugin_call_s* const call, const int64_t value) {
	return call->interface->make_int(call, value);
}

static inline mc_plugin_value_s mc_plugin_make_uint(mc_plugin_call_s* const call, const uint64_t value) {
	return call->interface->make_uint(call, value);
}

static inline mc_plugin_value_s mc_plugin_make_f64(mc_plugin_call_s* const call, const double value) {
	return call->interface->make_float(call, value);
}

static inline mc_plugin_value_s mc_plugin_make_bool(mc_plugin_call_s* const call, const bool value) {
	return call->interface->make_bool(call, value);
}

/// the result of a function without one.
static inline mc_plugin_value_s mc_plugin_make_none(mc_plugin_call_s* const call) {
	return call->interface->make_none(call);
}

/// the null (empty) slice.
static inline mc_plugin_value_s mc_plugin_make_null(mc_plugin_call_s* const call) {
	return call->interface->make_null(call);
}

/// a new heap allocation of 'count' elements, set to the zero value of
/// 'element'; '*data' (when not null) receives its memory. slices of it are
/// checked like slices of 'alloc': bounds, and use after mc_plugin_heap_free.
static inline mc_plugin_value_s mc_plugin_heap_alloc(mc_plugin_call_s* const call, const mc_plugin_type_s* const element, const uint64_t count,
													 void** const data) {
	return call->interface->heap_alloc(call, element, count, data);
}

/// the same for memory the plugin allocated itself ('count' elements of
/// 'element'); the machine calls 'release' when it is freed or the program ends.
static inline mc_plugin_value_s mc_plugin_heap_adopt(mc_plugin_call_s* const call, const mc_plugin_type_s* const element, const uint64_t count,
													 void* const memory, void (*release)(void* const memory)) {
	return call->interface->heap_adopt(call, element, count, memory, release);
}

/// what a slice argument views (stops the program when it is heap memory that was freed).
static inline mc_plugin_heap_e mc_plugin_heap_of(mc_plugin_call_s* const call, const size_t index) {
	return call->interface->heap_of(call, index);
}

/// frees the whole allocation a slice argument views; its slices stop working.
static inline void mc_plugin_heap_free(mc_plugin_call_s* const call, const size_t index) {
	call->interface->heap_free(call, index);
}

/// writes to the program's output.
static inline void mc_plugin_write(mc_plugin_call_s* const call, const char* const data, const size_t length) {
	call->interface->write(call, data, length);
}

/// stops the program with a runtime error; does not return.
MC_PLUGIN_NORETURN static inline void mc_plugin_fail(mc_plugin_call_s* const call, const char* const format, ...) MC_PLUGIN_PRINTF(2, 3);

MC_PLUGIN_NORETURN static inline void mc_plugin_fail(mc_plugin_call_s* const call, const char* const format, ...) {
	va_list args;
	va_start(args, format);
	call->interface->fail(call, format, args);
	va_end(args);
	for (;;) {}
}

// --- a call at compile time -----------------------------------------------------------------

/// the qualified name of the function called.
static inline const char* mc_plugin_check_name(mc_plugin_check_s* const check) {
	return check->interface->check_name(check);
}

static inline size_t mc_plugin_check_count(mc_plugin_check_s* const check) {
	return check->interface->check_count(check);
}

static inline size_t mc_plugin_check_fixed_count(mc_plugin_check_s* const check) {
	return check->interface->check_fixed_count(check);
}

/// the static type of an argument.
static inline const mc_plugin_type_s* mc_plugin_check_type(mc_plugin_check_s* const check, const size_t index) {
	return check->interface->check_type(check, index);
}

static inline size_t mc_plugin_check_type_argument_count(mc_plugin_check_s* const check) {
	return check->interface->check_type_argument_count(check);
}

static inline const mc_plugin_type_s* mc_plugin_check_type_argument(mc_plugin_check_s* const check, const size_t index) {
	return check->interface->check_type_argument(check, index);
}

/// the value of an argument when the compiler knows it.
static inline mc_plugin_constant_s mc_plugin_check_constant(mc_plugin_check_s* const check, const size_t index) {
	return check->interface->check_constant(check, index);
}

/// keeps 'flags' for the argument, for mc_plugin_argument_flags at run time.
static inline void mc_plugin_check_mark(mc_plugin_check_s* const check, const size_t index, const uint32_t flags) {
	check->interface->check_mark(check, index, flags);
}

/// reports an error (or a warning) at an argument (or MC_PLUGIN_CALL);
/// 'code' is a diagnostic code like "E0530", or null for the plugins' own
/// code. labels, notes and help are added to the returned diagnostic.
static inline mc_plugin_diagnostic_s* mc_plugin_error(mc_plugin_check_s* const check, const char* const code, const size_t index,
													  const char* const message) {
	return check->interface->report(check, false, code, index, message);
}

static inline mc_plugin_diagnostic_s* mc_plugin_warning(mc_plugin_check_s* const check, const char* const code, const size_t index,
														const char* const message) {
	return check->interface->report(check, true, code, index, message);
}

/// a label on an argument (or MC_PLUGIN_CALL); the first label of the
/// reported argument is the main one.
static inline void mc_plugin_label(mc_plugin_check_s* const check, mc_plugin_diagnostic_s* const diagnostic, const size_t index,
								   const char* const text) {
	check->interface->label(diagnostic, index, text);
}

static inline void mc_plugin_note(mc_plugin_check_s* const check, mc_plugin_diagnostic_s* const diagnostic, const char* const text) {
	check->interface->note(diagnostic, text);
}

static inline void mc_plugin_help(mc_plugin_check_s* const check, mc_plugin_diagnostic_s* const diagnostic, const char* const text) {
	check->interface->help(diagnostic, text);
}

/// the entry point of a plugin library: registers its functions; false
/// when it cannot (for example when mc_plugin_compatible says no).
MC_PLUGIN_EXPORT bool mc_plugin_init(mc_plugin_api_s* const api);

typedef bool (*mc_plugin_init_f)(mc_plugin_api_s* const api);

#define MC_PLUGIN_INIT_NAME "mc_plugin_init"

#endif
