
/// bytecode as text: listings and profile reports (see listing.h).

#include "mia/listing.h"
#include "mia/opcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// what a listing reads besides the module: the source files, read once each.
typedef struct {
	const mia_bytecode_module_s* module ;
	mia_source_table_s*          sources;
	const mia_source_s**         files  ;   // per file of the module
	bool*                        tried  ;
} _mia_listing_s;

static _mia_listing_s _mia_listing_open(const mia_bytecode_module_s* const module, mia_source_table_s* const sources) {
	const size_t count = (module->files.count > 0) ? module->files.count : 1;
	return (_mia_listing_s) {
		.module = module, .sources = sources, .files = mia_calloc(count, sizeof(const mia_source_s*)), .tried = mia_calloc(count, sizeof(bool)),
	};
}

static void _mia_listing_close(_mia_listing_s* const l) {
	mia_free((void*)l->files);
	mia_free(l->tried);
}

static const mia_source_s* _mia_listing_source(_mia_listing_s* const l, const int32_t file) {
	if (NULL == l->sources || file < 0 || (size_t)file >= l->module->files.count) return NULL;
	if (!l->tried[file]) {
		l->tried[file] = true;
		// the path comes from the bytecode: only a source file is read, not any file it names
		const char* const path = l->module->files.data[file];
		const size_t length = strlen(path);
		if (length < 4 || 0 != strcmp(path + length - 4, ".mia")) return NULL;
		for (size_t i = 0; i < l->sources->files.count && NULL == l->files[file]; ++i) {
			if (0 == strcmp(l->sources->files.data[i]->path, path)) l->files[file] = l->sources->files.data[i];
		}
		if (NULL == l->files[file]) l->files[file] = mia_source_load(l->sources, path);
	}
	return l->files[file];
}

/// appends 'file:line' and the text of the line (without its indentation) when it can be read.
static void _mia_listing_line(_mia_listing_s* const l, const int32_t file, const uint32_t line, mia_string_s* const out) {
	if (file < 0 || (size_t)file >= l->module->files.count) {
		mia_string_append_cstr(out, "(no position)");
		return;
	}
	mia_string_appendf(out, "%s:%u", l->module->files.data[file], line);
	const mia_source_s* const source = _mia_listing_source(l, file);
	if (NULL == source || 0 == line || line > source->lines.count) return;
	mia_view_s text = mia_source_line(source, line);
	while (text.length > 0 && (' ' == text.data[0] || '\t' == text.data[0])) {
		text.data += 1;
		text.length -= 1;
	}
	while (text.length > 0 && NULL != strchr(" \t\r\n", text.data[text.length - 1])) text.length -= 1;
	if (text.length > 0) mia_string_appendf(out, "  " mia_view_fmt, mia_view_arg(text));
}

// --- instructions ------------------------------------------------------------------------------

static void _mia_listing_name(const mia_opcode_e opcode, const mia_code_e type, const mia_code_e target, mia_string_s* const out) {
	if (opcode >= mia_opcode_count) {
		mia_string_appendf(out, "OPCODE_%d", (int)opcode);
		return;
	}
	mia_string_append_cstr(out, mia_opcode_name(opcode));
	const int types = mia_opcode_types(opcode);
	if (types >= 1) mia_string_appendf(out, "_%s", (type < mia_code_count) ? mia_code_name(type) : "?");
	if (types >= 2) mia_string_appendf(out, "_%s", (target < mia_code_count) ? mia_code_name(target) : "?");
}

void mia_listing_instruction(const mia_bytecode_instruction_s* const instruction, mia_string_s* const out) {
	_mia_listing_name(instruction->opcode, instruction->type, instruction->target, out);
	if (instruction->opcode >= mia_opcode_count) return;
	const int64_t operands[3] = { instruction->a, instruction->b, instruction->c, };
	for (int i = 0; i < mia_opcode_operands(instruction->opcode); ++i) mia_string_appendf(out, " %lld", (long long)operands[i]);
}

/// an instruction's name with its type codes as one number (and back), to count by it.
static uint64_t _mia_listing_key(const mia_bytecode_instruction_s* const instruction) {
	const int types = (instruction->opcode < mia_opcode_count) ? mia_opcode_types(instruction->opcode) : 0;
	const uint64_t type = (types >= 1) ? (uint64_t)instruction->type : 0, target = (types >= 2) ? (uint64_t)instruction->target : 0;
	return ((uint64_t)instruction->opcode * mia_code_count + type) * mia_code_count + target;
}

static void _mia_listing_key_name(const uint64_t key, mia_string_s* const out) {
	const uint64_t codes = mia_code_count;
	_mia_listing_name((mia_opcode_e)(key / (codes * codes)), (mia_code_e)(key / codes % codes), (mia_code_e)(key % codes), out);
}

// --- constants ---------------------------------------------------------------------------------

/// how long a constant is shown, in bytes; a longer one ends in '...'.
#define _mia_listing_constant_limit 60

static void _mia_listing_text(mia_string_s* const out, const char* const data, const size_t length) {
	mia_string_append_char(out, '"');
	for (size_t i = 0; i < length && out->length <= _mia_listing_constant_limit + 8; ++i) {
		const uint8_t c = (uint8_t)data[i];
		if ('"' == c || '\\' == c) mia_string_appendf(out, "\\%c", c);
		else if ('\n' == c) mia_string_append_cstr(out, "\\n");
		else if ('\t' == c) mia_string_append_cstr(out, "\\t");
		else if (c < 0x20 || 0x7F == c) mia_string_appendf(out, "\\x%02X", c);
		else mia_string_append_char(out, (char)c);
	}
	mia_string_append_char(out, '"');
}

static const mia_bytecode_type_s* _mia_listing_type(const mia_bytecode_module_s* const module, const int64_t type) {
	return (type >= 0 && (uint64_t)type < module->types.count) ? &module->types.data[type] : NULL;
}

static void _mia_listing_constant(const mia_bytecode_module_s* const module, const mia_bytecode_constant_s* const constant, mia_string_s* const out) {
	if (out->length > _mia_listing_constant_limit) return;
	switch (constant->kind) {
		case mia_bytecode_constant_none:    mia_string_append_cstr(out, "{}"); return;
		case mia_bytecode_constant_integer:
			if (constant->is_signed) mia_string_appendf(out, "%lld", (long long)constant->integer);
			else mia_string_appendf(out, "%llu", (unsigned long long)constant->integer);
			return;
		case mia_bytecode_constant_real: {
			char text[40];
			(void)snprintf(text, sizeof(text), "%.15g", constant->real);
			if (strtod(text, NULL) != constant->real) (void)snprintf(text, sizeof(text), "%.17g", constant->real);
			mia_string_append_cstr(out, text);
			if (NULL == strpbrk(text, ".ein")) mia_string_append_cstr(out, ".0");   // not 'inf', 'nan' or '1e+20'
			return;
		}
		case mia_bytecode_constant_boolean: mia_string_append_cstr(out, constant->boolean ? "true" : "false"); return;
		case mia_bytecode_constant_text: {
			const char* const text = (constant->text != NULL) ? constant->text : "";
			_mia_listing_text(out, text, strlen(text));
			return;
		}
		case mia_bytecode_constant_function:
			if (constant->integer < module->functions.count) mia_string_append_cstr(out, module->functions.data[constant->integer].name);
			else mia_string_appendf(out, "function %llu", (unsigned long long)constant->integer);
			return;
		case mia_bytecode_constant_intrinsic:
			if (constant->integer < module->intrinsics.count) mia_string_append_cstr(out, module->intrinsics.data[constant->integer].name);
			else mia_string_appendf(out, "intrinsic %llu", (unsigned long long)constant->integer);
			return;
		case mia_bytecode_constant_list: case mia_bytecode_constant_slice: {
			const bool slice = mia_bytecode_constant_slice == constant->kind;
			size_t start = 0, count = constant->items.count;
			if (slice) {
				if (constant->null) {
					mia_string_append_cstr(out, "{}");
					return;
				}
				const bool inside = constant->start >= 0 && constant->length >= 0 && (uint64_t)constant->start <= count
					&& (uint64_t)constant->length <= count - (size_t)constant->start;
				start = inside ? (size_t)constant->start : 0;
				count = inside ? (size_t)constant->length : 0;
			}
			// an array or a slice of u8 (a string) as text
			const mia_bytecode_type_s* const type = _mia_listing_type(module, constant->type);
			const bool sequence = type != NULL && (mia_bytecode_descriptor_array == type->kind || mia_bytecode_descriptor_slice == type->kind);
			const mia_bytecode_type_s* const element = sequence ? _mia_listing_type(module, type->element) : NULL;
			if (element != NULL && mia_code_u8 == element->code) {
				mia_string_s text = {0};
				for (size_t i = start; i < start + count; ++i) mia_string_append_char(&text, (char)constant->items.data[i].integer);
				if (slice) mia_string_append_cstr(out, "[..]");
				_mia_listing_text(out, text.data, text.length);
				mia_string_free(&text);
				return;
			}
			mia_string_append_cstr(out, slice ? "[..]{" : "{");
			for (size_t i = start; i < start + count && out->length <= _mia_listing_constant_limit; ++i) {
				if (i > start) mia_string_append_cstr(out, ", ");
				_mia_listing_constant(module, &constant->items.data[i], out);
			}
			mia_string_append_char(out, '}');
			return;
		}
	}
}

/// appends a constant, cut to _mia_listing_constant_limit bytes (not inside a code point).
static void _mia_listing_constant_at(const mia_bytecode_module_s* const module, const int64_t index, mia_string_s* const out) {
	if (index < 0 || (uint64_t)index >= module->constants.count) {
		mia_string_appendf(out, "constant %lld", (long long)index);
		return;
	}
	mia_string_s text = {0};
	_mia_listing_constant(module, &module->constants.data[index], &text);
	if (text.length > _mia_listing_constant_limit) {
		size_t cut = _mia_listing_constant_limit;
		while (cut > 0 && 0x80 == ((uint8_t)text.data[cut] & 0xC0)) --cut;
		text.length = cut;
		mia_string_append_cstr(&text, "...");
	}
	mia_string_append(out, mia_string_view(&text));
	mia_string_free(&text);
}

// --- the listing -------------------------------------------------------------------------------

static const char* const _mia_listing_kinds[mia_bytecode_descriptor_count] = {
	[mia_bytecode_descriptor_int] = "int", [mia_bytecode_descriptor_float] = "float", [mia_bytecode_descriptor_bool] = "bool",
	[mia_bytecode_descriptor_array] = "array", [mia_bytecode_descriptor_slice] = "slice", [mia_bytecode_descriptor_struct] = "struct",
	[mia_bytecode_descriptor_function] = "function",
};

static const char* _mia_listing_type_name(const mia_bytecode_module_s* const module, const int64_t index) {
	const mia_bytecode_type_s* const type = _mia_listing_type(module, index);
	return (type != NULL && type->name != NULL) ? type->name : "?";
}

static void _mia_listing_types(const mia_bytecode_module_s* const module, mia_string_s* const out) {
	mia_string_append_cstr(out, "types\n");
	for (size_t i = 0; i < module->types.count; ++i) {
		const mia_bytecode_type_s* const type = &module->types.data[i];
		const char* const kind = (type->kind < mia_bytecode_descriptor_count) ? _mia_listing_kinds[type->kind] : "?";
		mia_string_appendf(out, "  %4zu  %s: %s, %llu byte(s)", i, (type->name != NULL) ? type->name : "?", kind, (unsigned long long)type->size);
		if (mia_bytecode_descriptor_array == type->kind) {
			mia_string_appendf(out, ", %lld of %s", (long long)type->length, _mia_listing_type_name(module, type->element));
		}
		else if (mia_bytecode_descriptor_slice == type->kind) {
			mia_string_appendf(out, ", of %s", _mia_listing_type_name(module, type->element));
		}
		else if (mia_bytecode_descriptor_struct == type->kind) {
			for (size_t f = 0; f < type->field_types.count && f < type->field_names.count && f < type->field_offsets.count; ++f) {
				mia_string_appendf(out, "%s%s: %s at %llu", (0 == f) ? " (" : ", ", type->field_names.data[f],
					_mia_listing_type_name(module, type->field_types.data[f]), (unsigned long long)type->field_offsets.data[f]);
			}
			if (type->field_types.count > 0) mia_string_append_char(out, ')');
		}
		mia_string_append_char(out, '\n');
	}
}

/// what an instruction's operand names: the constant, the function or the plugin function.
static void _mia_listing_note(const mia_bytecode_module_s* const module, const mia_bytecode_instruction_s* const instruction, mia_string_s* const out) {
	const int64_t a = instruction->a;
	switch (instruction->opcode) {
		case mia_opcode_CONST: case mia_opcode_LOC_CONST: case mia_opcode_ASSERT_FAIL:
			_mia_listing_constant_at(module, a, out);
			break;
		case mia_opcode_CALL: case mia_opcode_FUNCTION:
			if (a >= 0 && (uint64_t)a < module->functions.count) mia_string_append_cstr(out, module->functions.data[a].name);
			break;
		case mia_opcode_CALL_INTRINSIC: case mia_opcode_INTRINSIC:
			if (a >= 0 && (uint64_t)a < module->intrinsics.count) mia_string_append_cstr(out, module->intrinsics.data[a].name);
			break;
		default:
			break;
	}
}

/// where an instruction can jump to, or -1.
static int64_t _mia_listing_target(const mia_bytecode_instruction_s* const instruction) {
	switch (instruction->opcode) {
		case mia_opcode_JUMP: case mia_opcode_JUMP_IF_FALSE: case mia_opcode_JUMP_IF_TRUE: return instruction->a;
		case mia_opcode_RANGE_TEST: case mia_opcode_RANGE_NEXT: return instruction->b;
		default: return -1;
	}
}

static void _mia_listing_function(_mia_listing_s* const l, const size_t index, mia_string_s* const out) {
	const mia_bytecode_function_s* const function = &l->module->functions.data[index];
	mia_string_appendf(out, "\nfunction %zu %s (%u parameter(s), frame %llu byte(s), result %u word(s))\n", index, function->name, function->parameters,
		(unsigned long long)function->frame, function->result);
	const size_t count = function->code.count;
	bool* const targets = mia_calloc(count > 0 ? count : 1, sizeof(bool));
	for (size_t i = 0; i < count; ++i) {
		const int64_t target = _mia_listing_target(&function->code.data[i]);
		if (target >= 0 && (uint64_t)target < count) targets[target] = true;
	}
	int32_t file = -1;
	uint32_t line = 0;
	mia_string_s note = {0};
	for (size_t i = 0; i < count; ++i) {
		const mia_bytecode_instruction_s* const instruction = &function->code.data[i];
		if (i < function->positions.count) {
			const mia_bytecode_position_s position = function->positions.data[i];
			if (position.file >= 0 && (position.file != file || position.line != line)) {
				file = position.file;
				line = position.line;
				mia_string_append_cstr(out, "          # ");
				_mia_listing_line(l, file, line, out);
				mia_string_append_char(out, '\n');
			}
		}
		const size_t start = out->length;
		mia_string_appendf(out, "  %c %5zu  ", targets[i] ? '>' : ' ', i);
		mia_listing_instruction(instruction, out);
		mia_string_clear(&note);
		_mia_listing_note(l->module, instruction, &note);
		if (note.length > 0) {
			// the notes in a column
			const size_t length = out->length - start;
			mia_string_append_repeat(out, ' ', (length < 40) ? 40 - length : 2);
			mia_string_appendf(out, "# %s", mia_string_cstr(&note));
		}
		mia_string_append_char(out, '\n');
	}
	mia_string_free(&note);
	mia_free(targets);
}

void mia_listing_module(const mia_bytecode_module_s* const module, mia_source_table_s* const sources, mia_string_s* const out) {
	_mia_listing_s l = _mia_listing_open(module, sources);
	mia_string_append_cstr(out, "files\n");
	for (size_t i = 0; i < module->files.count; ++i) mia_string_appendf(out, "  %4zu  %s\n", i, module->files.data[i]);
	_mia_listing_types(module, out);
	mia_string_append_cstr(out, "intrinsics\n");
	for (size_t i = 0; i < module->intrinsics.count; ++i) {
		const mia_bytecode_intrinsic_s* const intrinsic = &module->intrinsics.data[i];
		mia_string_appendf(out, "  %4zu  %s (plugin %s)\n", i, intrinsic->name, intrinsic->plugin);
	}
	if (module->entry >= 0 && (uint64_t)module->entry < module->functions.count) {
		mia_string_appendf(out, "entry\n  %s%s\n", module->functions.data[module->entry].name, module->entry_returns ? " (returns a value)" : "");
	}
	for (size_t i = 0; i < module->functions.count; ++i) _mia_listing_function(&l, i, out);
	_mia_listing_close(&l);
}

// --- the profile -------------------------------------------------------------------------------

/// one row of a table of the report: what it counts ('first' and 'second')
/// and how often; 'calls' for functions.
typedef struct {
	uint64_t first ;
	uint64_t second;
	uint64_t count ;
	uint64_t calls ;
} _mia_listing_row_s;

typedef mia_vector_of(_mia_listing_row_s) _mia_listing_rows_s;

static int _mia_listing_by_key(const void* const left, const void* const right) {
	const _mia_listing_row_s* const a = left;
	const _mia_listing_row_s* const b = right;
	if (a->first != b->first) return (a->first < b->first) ? -1 : 1;
	if (a->second != b->second) return (a->second < b->second) ? -1 : 1;
	return 0;
}

static int _mia_listing_by_count(const void* const left, const void* const right) {
	const _mia_listing_row_s* const a = left;
	const _mia_listing_row_s* const b = right;
	if (a->count != b->count) return (a->count > b->count) ? -1 : 1;
	return _mia_listing_by_key(left, right);
}

/// adds up the rows of the same key, then puts the largest first.
static void _mia_listing_rank(_mia_listing_rows_s* const rows) {
	if (0 == rows->count) return;
	qsort(rows->data, rows->count, sizeof(_mia_listing_row_s), _mia_listing_by_key);
	size_t kept = 0;
	for (size_t i = 0; i < rows->count; ++i) {
		_mia_listing_row_s* const last = (kept > 0) ? &rows->data[kept - 1] : NULL;
		if (last != NULL && 0 == _mia_listing_by_key(last, &rows->data[i])) {
			last->count += rows->data[i].count;
			last->calls += rows->data[i].calls;
		}
		else rows->data[kept++] = rows->data[i];
	}
	rows->count = kept;
	qsort(rows->data, rows->count, sizeof(_mia_listing_row_s), _mia_listing_by_count);
}

static void _mia_listing_none(mia_string_s* const out, const _mia_listing_rows_s* const rows) {
	if (0 == rows->count) mia_string_append_cstr(out, "  (none)\n");
}

static void _mia_listing_share(mia_string_s* const out, const uint64_t count, const uint64_t total) {
	mia_string_appendf(out, "  %14llu %6.2f%%  ", (unsigned long long)count, (total > 0) ? 100.0 * (double)count / (double)total : 0.0);
}

void mia_listing_profile(const mia_bytecode_module_s* const module, const mia_vm_profile_s* const profile, mia_source_table_s* const sources,
						 const size_t top, mia_string_s* const out) {
	_mia_listing_s l = _mia_listing_open(module, sources);
	_mia_listing_rows_s functions = {0}, lines = {0}, names = {0}, pairs = {0};
	uint64_t total = 0, calls = 0;
	for (size_t f = 0; f < profile->functions.count && f < module->functions.count; ++f) {
		const mia_vm_profile_function_s* const counts = &profile->functions.data[f];
		const mia_bytecode_function_s* const function = &module->functions.data[f];
		uint64_t runs = 0;
		for (size_t i = 0; i < counts->count && i < function->code.count; ++i) {
			const uint64_t count = counts->runs[i];
			if (0 == count) continue;
			runs += count;
			const uint64_t key = _mia_listing_key(&function->code.data[i]);
			mia_vector_push(&names, ((_mia_listing_row_s) { .first = key, .count = count, }));
			if (i < function->positions.count) {
				const mia_bytecode_position_s position = function->positions.data[i];
				mia_vector_push(&lines, ((_mia_listing_row_s) { .first = (uint64_t)((int64_t)position.file + 1), .second = position.line, .count = count, }));
			}
			if (counts->followed[i] > 0 && i + 1 < function->code.count) {
				const uint64_t next = _mia_listing_key(&function->code.data[i + 1]);
				mia_vector_push(&pairs, ((_mia_listing_row_s) { .first = key, .second = next, .count = counts->followed[i], }));
			}
		}
		total += runs;
		calls += counts->calls;
		if (runs > 0 || counts->calls > 0) mia_vector_push(&functions, ((_mia_listing_row_s) { .first = f, .count = runs, .calls = counts->calls, }));
	}
	_mia_listing_rank(&functions);
	_mia_listing_rank(&lines);
	_mia_listing_rank(&names);
	_mia_listing_rank(&pairs);
	const size_t shown = (top > 0) ? top : SIZE_MAX;
	mia_string_appendf(out, "profile: %llu instruction(s) run, %llu call(s)\n", (unsigned long long)total, (unsigned long long)calls);
	mia_string_append_cstr(out, "\nfunctions (the instructions run in each, not in what it calls)\n");
	for (size_t i = 0; i < functions.count && i < shown; ++i) {
		const _mia_listing_row_s* const row = &functions.data[i];
		_mia_listing_share(out, row->count, total);
		mia_string_appendf(out, "%10llu call(s)  %s\n", (unsigned long long)row->calls, module->functions.data[row->first].name);
	}
	_mia_listing_none(out, &functions);
	mia_string_append_cstr(out, "\nlines\n");
	for (size_t i = 0; i < lines.count && i < shown; ++i) {
		_mia_listing_share(out, lines.data[i].count, total);
		_mia_listing_line(&l, (int32_t)((int64_t)lines.data[i].first - 1), (uint32_t)lines.data[i].second, out);
		mia_string_append_char(out, '\n');
	}
	_mia_listing_none(out, &lines);
	mia_string_append_cstr(out, "\ninstructions\n");
	for (size_t i = 0; i < names.count && i < shown; ++i) {
		_mia_listing_share(out, names.data[i].count, total);
		_mia_listing_key_name(names.data[i].first, out);
		mia_string_append_char(out, '\n');
	}
	_mia_listing_none(out, &names);
	mia_string_append_cstr(out, "\npairs (an instruction, then the next one of the code right after it)\n");
	for (size_t i = 0; i < pairs.count && i < shown; ++i) {
		_mia_listing_share(out, pairs.data[i].count, total);
		_mia_listing_key_name(pairs.data[i].first, out);
		mia_string_append_cstr(out, ", ");
		_mia_listing_key_name(pairs.data[i].second, out);
		mia_string_append_char(out, '\n');
	}
	_mia_listing_none(out, &pairs);
	mia_vector_free(&functions);
	mia_vector_free(&lines);
	mia_vector_free(&names);
	mia_vector_free(&pairs);
	_mia_listing_close(&l);
}
