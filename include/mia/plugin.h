
#ifndef __mia__plugin_h__
#define __mia__plugin_h__

/// the plugin api: how c code provides functions to mia programs.
///
/// a plugin is a shared library (or code compiled into a program that
/// embeds mia, see mia_plugins_add) with one entry point, 'mia_plugin_init',
/// which registers the functions it provides under their qualified names:
/// the dotted name of the mia module that declares them and the function's
/// name. each such function names its plugin, which mia looks up by that
/// name in the plugin directories; a plugin is best named like its module
/// and kept beside it (the standard library's io.mia and io.so):
///
///     # math.mia, in the program's directory
///     [plugin = "math"]
///     pub sqrt :: (x: f64) -> f64;
///
///     // math.c, built as a shared library math.so beside math.mia
///     #include "mia/plugin.h"
///     #include <math.h>
///
///     static mia_plugin_value_s _sqrt(mia_plugin_call_s* const call) {
///         return mia_plugin_make_f64(call, sqrt(mia_plugin_get_f64(call, 0)));
///     }
///
///     mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api) {
///         if (!mia_plugin_compatible(api, mia_plugin_api_version)) return false;
///         return mia_plugin_register(api, "math", "math.sqrt", _sqrt, NULL);
///     }
///
///     mia run -P <the directory of math.so> main.mia
///
/// the checker types every call from the mia declaration, so a function can
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
/// ('mia_plugin_get_memory') and the elements of a slice are the bytes a c
/// struct or array of the same fields would have (sizes, offsets and
/// alignments come from the type functions). slices and function values
/// inside such memory are read with 'mia_plugin_slice_at' and
/// 'mia_plugin_function_name_at'.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/// the version of this api. a plugin passes the version it was built with
/// to mia_plugin_compatible; mia refuses plugins that need a newer one.
#define mia_plugin_api_version 1

#if defined(_WIN32)
#	define mia_plugin_export __declspec(dllexport)
#elif defined(__GNUC__)
#	define mia_plugin_export __attribute__((visibility("default")))
#else
#	define mia_plugin_export
#endif

// mia formats what plugins give it, with c99's printf (also on mingw: gnu_printf)
#if defined(__GNUC__) && defined(__MINGW32__) && !defined(mia_portable)
#	define mia_plugin_printf(_format, _arguments) __attribute__((format(gnu_printf, _format, _arguments)))
#elif defined(__GNUC__) && !defined(mia_portable)
#	define mia_plugin_printf(_format, _arguments) __attribute__((format(printf, _format, _arguments)))
#else
#	define mia_plugin_printf(_format, _arguments)
#endif

#if defined(_MSC_VER)
#	define mia_plugin_noreturn __declspec(noreturn)
#else
#	define mia_plugin_noreturn _Noreturn
#endif

typedef struct mia_plugin_interface_s mia_plugin_interface_s;

/// the handle of the registration (given to mia_plugin_init).
typedef struct {
	const mia_plugin_interface_s* interface;
} mia_plugin_api_s;

/// the handle of one call at run time.
typedef struct {
	const mia_plugin_interface_s* interface;
} mia_plugin_call_s;

/// the handle of one call while it is checked at compile time.
typedef struct {
	const mia_plugin_interface_s* interface;
} mia_plugin_check_s;

/// a type (opaque; ask the type functions).
typedef struct mia_plugin_type_s mia_plugin_type_s;

/// a diagnostic being reported by a check (opaque).
typedef struct mia_plugin_diagnostic_s mia_plugin_diagnostic_s;

/// the result of a call: made by the mia_plugin_make_* functions (opaque).
typedef struct {
	uint32_t kind    ;
	uint64_t words[2];
} mia_plugin_value_s;

typedef enum {
	mia_plugin_kind_int     ,   // s8 .. u64, int, uint
	mia_plugin_kind_float   ,   // f32, f64, float
	mia_plugin_kind_bool    ,
	mia_plugin_kind_array   ,
	mia_plugin_kind_slice   ,
	mia_plugin_kind_struct  ,
	mia_plugin_kind_function,
} mia_plugin_kind_e;

/// a slice argument: its elements (null when it is empty), how many, their
/// type, and whether it may be written through.
typedef struct {
	void*                    data     ;
	uint64_t                 length   ;
	const mia_plugin_type_s* element  ;
	bool                     read_only;
} mia_plugin_slice_s;

/// what a slice argument views, for functions that manage heap memory.
typedef enum {
	mia_plugin_heap_none ,   // a slice without memory (empty)
	mia_plugin_heap_whole,   // a whole allocation (of mia_plugin_heap_alloc)
	mia_plugin_heap_part ,   // a part of an allocation
	mia_plugin_heap_other,   // memory that is not heap memory
} mia_plugin_heap_e;

/// the value of an argument that the compiler knows.
typedef enum {
	mia_plugin_constant_none   ,   // not known at compile time
	mia_plugin_constant_integer,
	mia_plugin_constant_float  ,
	mia_plugin_constant_bool   ,
	mia_plugin_constant_bytes  ,   // a []u8 or [N]u8
	mia_plugin_constant_other  ,   // known, but none of the above
} mia_plugin_constant_kind_e;

typedef struct {
	mia_plugin_constant_kind_e kind    ;
	int64_t                    integer ;   // the bits of the value (unsigned types: read as uint64_t)
	double                     real    ;
	bool                       boolean ;
	const uint8_t*             bytes   ;   // valid during the check
	size_t                     length  ;
	bool                       c_string;   // the argument comes from a "..."c literal (also when its value is not known)
} mia_plugin_constant_s;

/// the run function of a plugin function, and its optional check.
typedef mia_plugin_value_s (*mia_plugin_run_f)(mia_plugin_call_s* const call);
typedef void (*mia_plugin_check_f)(mia_plugin_check_s* const check);

/// the argument index that means the whole call (in reports and labels).
#define mia_plugin_call ((size_t)-1)

struct mia_plugin_interface_s {
	uint32_t version;
	uint32_t size   ;   // of this table, in bytes
	// registration
	bool (*compatible)(mia_plugin_api_s* const api, const uint32_t version);
	bool (*register_function)(mia_plugin_api_s* const api, const char* const plugin, const char* const name, const mia_plugin_run_f run,
							  const mia_plugin_check_f check);
	// types
	mia_plugin_kind_e (*type_kind)(const mia_plugin_type_s* const type);
	const char* (*type_name)(const mia_plugin_type_s* const type);
	uint64_t (*type_size)(const mia_plugin_type_s* const type);
	uint64_t (*type_alignment)(const mia_plugin_type_s* const type);
	uint32_t (*type_bits)(const mia_plugin_type_s* const type);
	bool (*type_signed)(const mia_plugin_type_s* const type);
	const mia_plugin_type_s* (*type_element)(const mia_plugin_type_s* const type);
	uint64_t (*type_length)(const mia_plugin_type_s* const type);
	size_t (*type_field_count)(const mia_plugin_type_s* const type);
	const char* (*type_field_name)(const mia_plugin_type_s* const type, const size_t index);
	const mia_plugin_type_s* (*type_field_type)(const mia_plugin_type_s* const type, const size_t index);
	uint64_t (*type_field_offset)(const mia_plugin_type_s* const type, const size_t index);
	// a call at run time
	size_t (*count)(mia_plugin_call_s* const call);
	size_t (*fixed_count)(mia_plugin_call_s* const call);
	const mia_plugin_type_s* (*argument_type)(mia_plugin_call_s* const call, const size_t index);
	size_t (*type_argument_count)(mia_plugin_call_s* const call);
	const mia_plugin_type_s* (*type_argument)(mia_plugin_call_s* const call, const size_t index);
	uint32_t (*argument_flags)(mia_plugin_call_s* const call, const size_t index);
	int64_t (*get_int)(mia_plugin_call_s* const call, const size_t index);
	uint64_t (*get_uint)(mia_plugin_call_s* const call, const size_t index);
	double (*get_float)(mia_plugin_call_s* const call, const size_t index);
	bool (*get_bool)(mia_plugin_call_s* const call, const size_t index);
	const void* (*get_memory)(mia_plugin_call_s* const call, const size_t index);
	mia_plugin_slice_s (*get_slice)(mia_plugin_call_s* const call, const size_t index);
	uint64_t (*slice_length)(mia_plugin_call_s* const call, const size_t index);
	bool (*slice_read_only)(mia_plugin_call_s* const call, const size_t index);
	mia_plugin_slice_s (*slice_at)(mia_plugin_call_s* const call, const void* const memory, const mia_plugin_type_s* const type);
	const char* (*function_name_at)(mia_plugin_call_s* const call, const void* const memory);
	mia_plugin_value_s (*make_int)(mia_plugin_call_s* const call, const int64_t value);
	mia_plugin_value_s (*make_uint)(mia_plugin_call_s* const call, const uint64_t value);
	mia_plugin_value_s (*make_float)(mia_plugin_call_s* const call, const double value);
	mia_plugin_value_s (*make_bool)(mia_plugin_call_s* const call, const bool value);
	mia_plugin_value_s (*make_none)(mia_plugin_call_s* const call);
	mia_plugin_value_s (*make_empty)(mia_plugin_call_s* const call);
	mia_plugin_value_s (*heap_alloc)(mia_plugin_call_s* const call, const mia_plugin_type_s* const element, const uint64_t count, void** const data);
	mia_plugin_value_s (*heap_adopt)(mia_plugin_call_s* const call, const mia_plugin_type_s* const element, const uint64_t count, void* const memory,
									 void (*release)(void* const memory));
	mia_plugin_heap_e (*heap_of)(mia_plugin_call_s* const call, const size_t index);
	void (*heap_free)(mia_plugin_call_s* const call, const size_t index);
	void (*write)(mia_plugin_call_s* const call, const char* const data, const size_t length);
	void (*fail)(mia_plugin_call_s* const call, const char* const format, va_list args);
	// a call at compile time
	const char* (*check_name)(mia_plugin_check_s* const check);
	size_t (*check_count)(mia_plugin_check_s* const check);
	size_t (*check_fixed_count)(mia_plugin_check_s* const check);
	const mia_plugin_type_s* (*check_type)(mia_plugin_check_s* const check, const size_t index);
	size_t (*check_type_argument_count)(mia_plugin_check_s* const check);
	const mia_plugin_type_s* (*check_type_argument)(mia_plugin_check_s* const check, const size_t index);
	mia_plugin_constant_s (*check_constant)(mia_plugin_check_s* const check, const size_t index);
	void (*check_mark)(mia_plugin_check_s* const check, const size_t index, const uint32_t flags);
	mia_plugin_diagnostic_s* (*report)(mia_plugin_check_s* const check, const bool warning, const char* const code, const size_t index,
									   const char* const message);
	void (*label)(mia_plugin_diagnostic_s* const diagnostic, const size_t index, const char* const text);
	void (*note)(mia_plugin_diagnostic_s* const diagnostic, const char* const text);
	void (*help)(mia_plugin_diagnostic_s* const diagnostic, const char* const text);
};

// --- registration -------------------------------------------------------------------------

/// whether this mia can run a plugin built with api 'version'; when it
/// cannot, mia_plugin_init should return false (mia explains why).
static inline bool mia_plugin_compatible(mia_plugin_api_s* const api, const uint32_t version) {
	return api->interface->compatible(api, version);
}

/// provides the function 'name' ('module.function') as part of 'plugin';
/// 'check' may be null. false when the name is provided already.
static inline bool mia_plugin_register(mia_plugin_api_s* const api, const char* const plugin, const char* const name, const mia_plugin_run_f run,
									   const mia_plugin_check_f check) {
	return api->interface->register_function(api, plugin, name, run, check);
}

// --- types (at run time and at compile time) ----------------------------------------------

#define _mia_plugin_type_call(_handle, _function, ...) ((_handle)->interface->_function(__VA_ARGS__))

/// the types of arguments come from mia_plugin_argument_type or
/// mia_plugin_check_type; these functions need the call or check handle
/// only to reach the function table.
#define mia_plugin_type_kind(_handle, _type)            _mia_plugin_type_call(_handle, type_kind, _type)
#define mia_plugin_type_name(_handle, _type)            _mia_plugin_type_call(_handle, type_name, _type)
#define mia_plugin_type_size(_handle, _type)            _mia_plugin_type_call(_handle, type_size, _type)
#define mia_plugin_type_alignment(_handle, _type)       _mia_plugin_type_call(_handle, type_alignment, _type)
#define mia_plugin_type_bits(_handle, _type)            _mia_plugin_type_call(_handle, type_bits, _type)
#define mia_plugin_type_signed(_handle, _type)          _mia_plugin_type_call(_handle, type_signed, _type)
#define mia_plugin_type_element(_handle, _type)         _mia_plugin_type_call(_handle, type_element, _type)
#define mia_plugin_type_length(_handle, _type)          _mia_plugin_type_call(_handle, type_length, _type)
#define mia_plugin_type_field_count(_handle, _type)     _mia_plugin_type_call(_handle, type_field_count, _type)
#define mia_plugin_type_field_name(_handle, _type, _i)  _mia_plugin_type_call(_handle, type_field_name, _type, _i)
#define mia_plugin_type_field_type(_handle, _type, _i)  _mia_plugin_type_call(_handle, type_field_type, _type, _i)
#define mia_plugin_type_field_offset(_handle, _type, _i) _mia_plugin_type_call(_handle, type_field_offset, _type, _i)

// --- a call at run time ---------------------------------------------------------------------

/// the number of arguments (the values given for '...' included), and of
/// the fixed ones (the variadic values follow them).
static inline size_t mia_plugin_count(mia_plugin_call_s* const call) {
	return call->interface->count(call);
}

static inline size_t mia_plugin_fixed_count(mia_plugin_call_s* const call) {
	return call->interface->fixed_count(call);
}

static inline const mia_plugin_type_s* mia_plugin_argument_type(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->argument_type(call, index);
}

/// the type arguments of a generic function ('T' of 'alloc<T>').
static inline size_t mia_plugin_type_argument_count(mia_plugin_call_s* const call) {
	return call->interface->type_argument_count(call);
}

static inline const mia_plugin_type_s* mia_plugin_type_argument(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->type_argument(call, index);
}

/// what the check marked on an argument at compile time (0 when nothing).
static inline uint32_t mia_plugin_argument_flags(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->argument_flags(call, index);
}

/// an integer, float or bool argument (any width; f32 is widened).
static inline int64_t mia_plugin_get_int(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->get_int(call, index);
}

static inline uint64_t mia_plugin_get_uint(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->get_uint(call, index);
}

static inline double mia_plugin_get_f64(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->get_float(call, index);
}

static inline bool mia_plugin_get_bool(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->get_bool(call, index);
}

/// the bytes of any argument, laid out like c (a struct, an array, a scalar).
static inline const void* mia_plugin_get_memory(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->get_memory(call, index);
}

/// a slice argument (stops the program when its memory is no longer alive).
static inline mia_plugin_slice_s mia_plugin_get_slice(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->get_slice(call, index);
}

/// the length and the read-only flag of a slice argument, without touching its memory.
static inline uint64_t mia_plugin_slice_length(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->slice_length(call, index);
}

static inline bool mia_plugin_slice_read_only(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->slice_read_only(call, index);
}

/// a slice of 'type' stored in memory (a field, an element).
static inline mia_plugin_slice_s mia_plugin_slice_at(mia_plugin_call_s* const call, const void* const memory, const mia_plugin_type_s* const type) {
	return call->interface->slice_at(call, memory, type);
}

/// the name of the function value stored in memory.
static inline const char* mia_plugin_function_name_at(mia_plugin_call_s* const call, const void* const memory) {
	return call->interface->function_name_at(call, memory);
}

/// results: converted to the declared result type (integers wrap, f32 rounds).
static inline mia_plugin_value_s mia_plugin_make_int(mia_plugin_call_s* const call, const int64_t value) {
	return call->interface->make_int(call, value);
}

static inline mia_plugin_value_s mia_plugin_make_uint(mia_plugin_call_s* const call, const uint64_t value) {
	return call->interface->make_uint(call, value);
}

static inline mia_plugin_value_s mia_plugin_make_f64(mia_plugin_call_s* const call, const double value) {
	return call->interface->make_float(call, value);
}

static inline mia_plugin_value_s mia_plugin_make_bool(mia_plugin_call_s* const call, const bool value) {
	return call->interface->make_bool(call, value);
}

/// the result of a function without one.
static inline mia_plugin_value_s mia_plugin_make_none(mia_plugin_call_s* const call) {
	return call->interface->make_none(call);
}

/// the empty slice (no memory, length 0; mia's '{}').
static inline mia_plugin_value_s mia_plugin_make_empty(mia_plugin_call_s* const call) {
	return call->interface->make_empty(call);
}

/// a new heap allocation of 'count' elements, set to the zero value of
/// 'element'; '*data' (when not null) receives its memory. slices of it are
/// checked like slices of 'alloc': bounds, and use after mia_plugin_heap_free.
static inline mia_plugin_value_s mia_plugin_heap_alloc(mia_plugin_call_s* const call, const mia_plugin_type_s* const element, const uint64_t count,
													   void** const data) {
	return call->interface->heap_alloc(call, element, count, data);
}

/// the same for memory the plugin allocated itself ('count' elements of
/// 'element'); the machine calls 'release' when it is freed or the program ends.
static inline mia_plugin_value_s mia_plugin_heap_adopt(mia_plugin_call_s* const call, const mia_plugin_type_s* const element, const uint64_t count,
													   void* const memory, void (*release)(void* const memory)) {
	return call->interface->heap_adopt(call, element, count, memory, release);
}

/// what a slice argument views (stops the program when it is heap memory that was freed).
static inline mia_plugin_heap_e mia_plugin_heap_of(mia_plugin_call_s* const call, const size_t index) {
	return call->interface->heap_of(call, index);
}

/// frees the whole allocation a slice argument views; its slices stop working.
static inline void mia_plugin_heap_free(mia_plugin_call_s* const call, const size_t index) {
	call->interface->heap_free(call, index);
}

/// writes to the program's output.
static inline void mia_plugin_write(mia_plugin_call_s* const call, const char* const data, const size_t length) {
	call->interface->write(call, data, length);
}

/// stops the program with a runtime error; does not return.
mia_plugin_noreturn static inline void mia_plugin_fail(mia_plugin_call_s* const call, const char* const format, ...) mia_plugin_printf(2, 3);

mia_plugin_noreturn static inline void mia_plugin_fail(mia_plugin_call_s* const call, const char* const format, ...) {
	va_list args;
	va_start(args, format);
	call->interface->fail(call, format, args);
	va_end(args);
	for (;;) {}
}

// --- a call at compile time -----------------------------------------------------------------

/// the qualified name of the function called.
static inline const char* mia_plugin_check_name(mia_plugin_check_s* const check) {
	return check->interface->check_name(check);
}

static inline size_t mia_plugin_check_count(mia_plugin_check_s* const check) {
	return check->interface->check_count(check);
}

static inline size_t mia_plugin_check_fixed_count(mia_plugin_check_s* const check) {
	return check->interface->check_fixed_count(check);
}

/// the static type of an argument.
static inline const mia_plugin_type_s* mia_plugin_check_type(mia_plugin_check_s* const check, const size_t index) {
	return check->interface->check_type(check, index);
}

static inline size_t mia_plugin_check_type_argument_count(mia_plugin_check_s* const check) {
	return check->interface->check_type_argument_count(check);
}

static inline const mia_plugin_type_s* mia_plugin_check_type_argument(mia_plugin_check_s* const check, const size_t index) {
	return check->interface->check_type_argument(check, index);
}

/// the value of an argument when the compiler knows it.
static inline mia_plugin_constant_s mia_plugin_check_constant(mia_plugin_check_s* const check, const size_t index) {
	return check->interface->check_constant(check, index);
}

/// keeps 'flags' for the argument, for mia_plugin_argument_flags at run time.
static inline void mia_plugin_check_mark(mia_plugin_check_s* const check, const size_t index, const uint32_t flags) {
	check->interface->check_mark(check, index, flags);
}

/// reports an error (or a warning) at an argument (or mia_plugin_call);
/// 'code' is a diagnostic code like "E0530", or null for the plugins' own
/// code. labels, notes and help are added to the returned diagnostic.
static inline mia_plugin_diagnostic_s* mia_plugin_error(mia_plugin_check_s* const check, const char* const code, const size_t index,
														const char* const message) {
	return check->interface->report(check, false, code, index, message);
}

static inline mia_plugin_diagnostic_s* mia_plugin_warning(mia_plugin_check_s* const check, const char* const code, const size_t index,
														  const char* const message) {
	return check->interface->report(check, true, code, index, message);
}

/// a label on an argument (or mia_plugin_call); the first label of the
/// reported argument is the main one.
static inline void mia_plugin_label(mia_plugin_check_s* const check, mia_plugin_diagnostic_s* const diagnostic, const size_t index,
									const char* const text) {
	check->interface->label(diagnostic, index, text);
}

static inline void mia_plugin_note(mia_plugin_check_s* const check, mia_plugin_diagnostic_s* const diagnostic, const char* const text) {
	check->interface->note(diagnostic, text);
}

static inline void mia_plugin_help(mia_plugin_check_s* const check, mia_plugin_diagnostic_s* const diagnostic, const char* const text) {
	check->interface->help(diagnostic, text);
}

/// the entry point of a plugin library: registers its functions; false
/// when it cannot (for example when mia_plugin_compatible says no).
mia_plugin_export bool mia_plugin_init(mia_plugin_api_s* const api);

typedef bool (*mia_plugin_init_f)(mia_plugin_api_s* const api);

#define mia_plugin_init_name "mia_plugin_init"

/// optional: called once before mia unloads the library, to give back what
/// the plugin keeps between calls (buffers, handles). a plugin that keeps
/// nothing leaves it out.
mia_plugin_export void mia_plugin_exit(void);

typedef void (*mia_plugin_exit_f)(void);

#define mia_plugin_exit_name "mia_plugin_exit"

#endif
