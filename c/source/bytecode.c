
/// bytecode: instruction names, the '.mcb' file format (json, shared with the
/// python implementation) and the disassembler.

#include "mc/bytecode.h"
#include "mc/source.h"
#include "mc/format.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _mc_magic "mc-bytecode 1"

typedef struct {
	const char* name ;
	char        shape;
} _mc_opcode_info_s;

static const _mc_opcode_info_s _mc_opcodes[] = {
#define mc_opcode(_name, _shape) { #_name, _shape, },
#include "mc/opcodes.def"
#undef mc_opcode
};

static const char* const _mc_type_codes[] = {
	"S8", "S16", "S32", "S64", "U8", "U16", "U32", "U64", "INT", "UINT", "F32", "F64", "FLOAT", "BOOL", "FN",
};

const char* mc_opcode_name(const mc_opcode_e opcode) {
	return _mc_opcodes[opcode].name;
}

const char* mc_type_code_name(const mc_type_code_e code) {
	return _mc_type_codes[code];
}

void mc_instruction_name(const mc_instruction_s* const instruction, mc_string_s* const out) {
	const _mc_opcode_info_s* const info = &_mc_opcodes[instruction->opcode];
	mc_string_append_cstr(out, info->name);
	if ('t' == info->shape) mc_string_appendf(out, "_%s", _mc_type_codes[instruction->type]);
	if ('c' == info->shape) mc_string_appendf(out, "_%s_%s", _mc_type_codes[instruction->type], _mc_type_codes[instruction->target]);
}

static bool _mc_type_code(const char* const text, const size_t length, mc_type_code_e* const code) {
	for (size_t i = 0; i < mc_count_of(_mc_type_codes); ++i) {
		if (strlen(_mc_type_codes[i]) == length && 0 == memcmp(_mc_type_codes[i], text, length)) {
			*code = (mc_type_code_e)i;
			return true;
		}
	}
	return false;
}

bool mc_instruction_parse(const char* const name, mc_instruction_s* const instruction) {
	*instruction = (mc_instruction_s) {0};
	for (size_t i = 0; i < mc_count_of(_mc_opcodes); ++i) {
		const _mc_opcode_info_s* const info = &_mc_opcodes[i];
		const size_t length = strlen(info->name);
		if (0 != strncmp(name, info->name, length)) continue;
		const char* const rest = name + length;
		if ('t' == info->shape || 'c' == info->shape) {
			if ('_' != rest[0]) continue;
			instruction->opcode = (mc_opcode_e)i;
			if ('t' == info->shape) return _mc_type_code(rest + 1, strlen(rest + 1), &instruction->type);
			const char* const second = strchr(rest + 1, '_');
			if (NULL == second) return false;
			return _mc_type_code(rest + 1, (size_t)(second - rest - 1), &instruction->type)
				&& _mc_type_code(second + 1, strlen(second + 1), &instruction->target);
		}
		if ('\0' != rest[0]) continue;
		instruction->opcode = (mc_opcode_e)i;
		return true;
	}
	return false;
}

void mc_bc_constant_free(mc_bc_constant_s* const constant) {
	for (size_t i = 0; i < constant->items.count; ++i) mc_bc_constant_free(&constant->items.data[i]);
	mc_vector_free(&constant->items);
	mc_free(constant->text);
	constant->text = NULL;
}

void mc_module_bc_free(mc_module_bc_s* const module) {
	for (size_t i = 0; i < module->functions.count; ++i) {
		mc_free(module->functions.data[i].name);
		mc_vector_free(&module->functions.data[i].code);
		mc_vector_free(&module->functions.data[i].positions);
	}
	mc_vector_free(&module->functions);
	for (size_t i = 0; i < module->constants.count; ++i) mc_bc_constant_free(&module->constants.data[i]);
	mc_vector_free(&module->constants);
	for (size_t i = 0; i < module->types.count; ++i) {
		mc_bc_type_s* const type = &module->types.data[i];
		mc_free(type->name);
		mc_free(type->kind);
		for (size_t f = 0; f < type->field_names.count; ++f) mc_free(type->field_names.data[f]);
		mc_vector_free(&type->field_names);
		mc_vector_free(&type->field_types);
	}
	mc_vector_free(&module->types);
	for (size_t i = 0; i < module->builtins.count; ++i) {
		mc_free(module->builtins.data[i].name);
		mc_vector_free(&module->builtins.data[i].types);
	}
	mc_vector_free(&module->builtins);
	for (size_t i = 0; i < module->natives.count; ++i) mc_free(module->natives.data[i]);
	mc_vector_free(&module->natives);
	for (size_t i = 0; i < module->files.count; ++i) mc_free(module->files.data[i]);
	mc_vector_free(&module->files);
}

// --- writing ---------------------------------------------------------------------------

static void _mc_json_string(mc_string_s* const out, const char* const text) {
	mc_string_append_char(out, '"');
	for (const uint8_t* cursor = (const uint8_t*)text; *cursor != '\0'; ++cursor) {
		const uint8_t c = *cursor;
		if ('"' == c || '\\' == c) mc_string_appendf(out, "\\%c", c);
		else if ('\n' == c) mc_string_append_cstr(out, "\\n");
		else if ('\t' == c) mc_string_append_cstr(out, "\\t");
		else if ('\r' == c) mc_string_append_cstr(out, "\\r");
		else if (c < 0x20) mc_string_appendf(out, "\\u%04x", c);
		else mc_string_append_char(out, (char)c);
	}
	mc_string_append_char(out, '"');
}

static void _mc_json_real(mc_string_s* const out, const double value) {
	if (isnan(value)) mc_string_append_cstr(out, "NaN");
	else if (isinf(value)) mc_string_append_cstr(out, value > 0 ? "Infinity" : "-Infinity");
	else mc_format_float_repr(out, value);
}

static void _mc_json_constant(mc_string_s* const out, const mc_bc_constant_s* const constant) {
	switch (constant->kind) {
		case mc_constant_none:     mc_string_append_cstr(out, "null"); return;
		case mc_constant_integer:  mc_format_int128(out, constant->integer); return;
		case mc_constant_real:     _mc_json_real(out, constant->real); return;
		case mc_constant_boolean:  mc_string_append_cstr(out, constant->boolean ? "true" : "false"); return;
		case mc_constant_text:     _mc_json_string(out, constant->text); return;
		case mc_constant_function: mc_string_appendf(out, "{\"function\":%lld}", (long long)constant->integer); return;
		case mc_constant_builtin:  mc_string_appendf(out, "{\"builtin\":%lld}", (long long)constant->integer); return;
		case mc_constant_list:
		case mc_constant_slice:
			if (mc_constant_slice == constant->kind) mc_string_append_cstr(out, "{\"slice\":");
			mc_string_append_char(out, '[');
			for (size_t i = 0; i < constant->items.count; ++i) {
				if (i > 0) mc_string_append_char(out, ',');
				_mc_json_constant(out, &constant->items.data[i]);
			}
			mc_string_append_char(out, ']');
			if (mc_constant_slice == constant->kind) {
				mc_string_appendf(out, ",\"start\":%lld,\"length\":%lld,\"read_only\":%s}", (long long)constant->start, (long long)constant->length,
					constant->read_only ? "true" : "false");
			}
			return;
	}
}

static void _mc_json_operand(mc_string_s* const out, const mc_instruction_s* const instruction) {
	if (!instruction->has_a) {
		mc_string_append_cstr(out, "null");
	}
	else if (instruction->has_b) {
		if (instruction->b_bool) mc_string_appendf(out, "[%lld,%s]", (long long)instruction->a, instruction->b ? "true" : "false");
		else mc_string_appendf(out, "[%lld,%lld]", (long long)instruction->a, (long long)instruction->b);
	}
	else if (mc_op_RANGE_TEST == instruction->opcode) {
		mc_string_appendf(out, "[%lld,null]", (long long)instruction->a);
	}
	else {
		mc_string_appendf(out, "%lld", (long long)instruction->a);
	}
}

bool mc_module_bc_save(const mc_module_bc_s* const module, const char* const path) {
	mc_string_s out = {0};
	mc_string_append_cstr(&out, _mc_magic "\n{\"functions\":[");
	for (size_t f = 0; f < module->functions.count; ++f) {
		const mc_bc_function_s* const function = &module->functions.data[f];
		if (f > 0) mc_string_append_char(&out, ',');
		mc_string_append_cstr(&out, "{\"name\":");
		_mc_json_string(&out, function->name);
		mc_string_appendf(&out, ",\"parameters\":%lld,\"locals\":%lld,\"returns\":%s,\"code\":[", (long long)function->parameters,
			(long long)function->locals, function->returns ? "true" : "false");
		for (size_t i = 0; i < function->code.count; ++i) {
			if (i > 0) mc_string_append_char(&out, ',');
			mc_string_append_cstr(&out, "[\"");
			mc_instruction_name(&function->code.data[i], &out);
			mc_string_append_cstr(&out, "\",");
			_mc_json_operand(&out, &function->code.data[i]);
			mc_string_append_char(&out, ']');
		}
		mc_string_append_cstr(&out, "],\"positions\":[");
		for (size_t i = 0; i < function->positions.count; ++i) {
			const mc_position_s* const position = &function->positions.data[i];
			if (i > 0) mc_string_append_char(&out, ',');
			if (position->file < 0) mc_string_append_cstr(&out, "null");
			else mc_string_appendf(&out, "[%d,%u,%u,%u]", position->file, position->line, position->column, position->length);
		}
		mc_string_append_cstr(&out, "]}");
	}
	mc_string_append_cstr(&out, "],\"constants\":[");
	for (size_t i = 0; i < module->constants.count; ++i) {
		if (i > 0) mc_string_append_char(&out, ',');
		_mc_json_constant(&out, &module->constants.data[i]);
	}
	mc_string_append_cstr(&out, "],\"types\":[");
	for (size_t i = 0; i < module->types.count; ++i) {
		const mc_bc_type_s* const type = &module->types.data[i];
		if (i > 0) mc_string_append_char(&out, ',');
		mc_string_append_cstr(&out, "{\"name\":");
		_mc_json_string(&out, type->name);
		mc_string_append_cstr(&out, ",\"kind\":");
		_mc_json_string(&out, type->kind);
		if (0 == strcmp(type->kind, "array")) mc_string_appendf(&out, ",\"element\":%lld,\"length\":%lld", (long long)type->element, (long long)type->length);
		if (0 == strcmp(type->kind, "slice")) mc_string_appendf(&out, ",\"element\":%lld", (long long)type->element);
		if (0 == strcmp(type->kind, "struct")) {
			mc_string_append_cstr(&out, ",\"fields\":[");
			for (size_t f = 0; f < type->field_names.count; ++f) {
				if (f > 0) mc_string_append_char(&out, ',');
				mc_string_append_char(&out, '[');
				_mc_json_string(&out, type->field_names.data[f]);
				mc_string_appendf(&out, ",%lld]", (long long)type->field_types.data[f]);
			}
			mc_string_append_char(&out, ']');
		}
		if (type->zero < 0) mc_string_append_cstr(&out, ",\"zero\":null}");
		else mc_string_appendf(&out, ",\"zero\":%lld}", (long long)type->zero);
	}
	mc_string_append_cstr(&out, "],\"builtins\":[");
	for (size_t i = 0; i < module->builtins.count; ++i) {
		if (i > 0) mc_string_append_char(&out, ',');
		mc_string_append_char(&out, '[');
		_mc_json_string(&out, module->builtins.data[i].name);
		mc_string_append_cstr(&out, ",[");
		for (size_t t = 0; t < module->builtins.data[i].types.count; ++t) {
			if (t > 0) mc_string_append_char(&out, ',');
			mc_string_appendf(&out, "%lld", (long long)module->builtins.data[i].types.data[t]);
		}
		mc_string_append_cstr(&out, "]]");
	}
	mc_string_append_cstr(&out, "],\"natives\":[");
	for (size_t i = 0; i < module->natives.count; ++i) {
		if (i > 0) mc_string_append_char(&out, ',');
		_mc_json_string(&out, module->natives.data[i]);
	}
	mc_string_append_cstr(&out, "],\"files\":[");
	for (size_t i = 0; i < module->files.count; ++i) {
		if (i > 0) mc_string_append_char(&out, ',');
		_mc_json_string(&out, module->files.data[i]);
	}
	mc_string_appendf(&out, "],\"entry\":%lld,\"entry_returns\":%s}\n", (long long)module->entry, module->entry_returns ? "true" : "false");
	FILE* const file = fopen(path, "wb");
	bool written = false;
	if (file != NULL) {
		written = fwrite(out.data, 1, out.length, file) == out.length;
		written = (0 == fclose(file)) && written;
	}
	mc_string_free(&out);
	return written;
}

// --- reading -----------------------------------------------------------------------------

typedef enum { _mc_json_kind_null, _mc_json_kind_bool, _mc_json_kind_integer, _mc_json_kind_real, _mc_json_kind_string, _mc_json_kind_array, _mc_json_kind_object, } _mc_json_kind_e;

typedef struct _mc_json_s _mc_json_s;

struct _mc_json_s {
	_mc_json_kind_e             kind   ;
	bool                        boolean;
	__int128                    integer;
	double                      real   ;
	char*                       text   ;   // strings
	mc_vector_of(_mc_json_s*)   items  ;   // arrays, object values
	mc_vector_of(char*)         keys   ;   // objects
};

typedef struct {
	const char* cursor;
	const char* error ;
} _mc_json_reader_s;

static void _mc_json_free(_mc_json_s* const value) {
	if (NULL == value) return;
	for (size_t i = 0; i < value->items.count; ++i) _mc_json_free(value->items.data[i]);
	for (size_t i = 0; i < value->keys.count; ++i) mc_free(value->keys.data[i]);
	mc_vector_free(&value->items);
	mc_vector_free(&value->keys);
	mc_free(value->text);
	mc_free(value);
}

static void _mc_json_space(_mc_json_reader_s* const r) {
	while (isspace((unsigned char)*r->cursor)) ++r->cursor;
}

static void _mc_utf8(mc_string_s* const out, const uint32_t point) {
	if (point < 0x80) {
		mc_string_append_char(out, (char)point);
	}
	else if (point < 0x800) {
		mc_string_append_char(out, (char)(0xC0 | (point >> 6)));
		mc_string_append_char(out, (char)(0x80 | (point & 0x3F)));
	}
	else if (point < 0x10000) {
		mc_string_append_char(out, (char)(0xE0 | (point >> 12)));
		mc_string_append_char(out, (char)(0x80 | ((point >> 6) & 0x3F)));
		mc_string_append_char(out, (char)(0x80 | (point & 0x3F)));
	}
	else {
		mc_string_append_char(out, (char)(0xF0 | (point >> 18)));
		mc_string_append_char(out, (char)(0x80 | ((point >> 12) & 0x3F)));
		mc_string_append_char(out, (char)(0x80 | ((point >> 6) & 0x3F)));
		mc_string_append_char(out, (char)(0x80 | (point & 0x3F)));
	}
}

static char* _mc_json_text(_mc_json_reader_s* const r) {
	++r->cursor;
	mc_string_s out = {0};
	while (*r->cursor != '"') {
		if ('\0' == *r->cursor) {
			r->error = "unterminated string";
			break;
		}
		if ('\\' != *r->cursor) {
			mc_string_append_char(&out, *r->cursor++);
			continue;
		}
		++r->cursor;
		const char escape = *r->cursor++;
		switch (escape) {
			case 'n': mc_string_append_char(&out, '\n'); break;
			case 't': mc_string_append_char(&out, '\t'); break;
			case 'r': mc_string_append_char(&out, '\r'); break;
			case 'b': mc_string_append_char(&out, '\b'); break;
			case 'f': mc_string_append_char(&out, '\f'); break;
			case 'u': {
				char digits[5] = {0};
				memcpy(digits, r->cursor, 4);
				r->cursor += 4;
				uint32_t point = (uint32_t)strtoul(digits, NULL, 16);
				if (point >= 0xD800 && point < 0xDC00 && '\\' == r->cursor[0] && 'u' == r->cursor[1]) {
					memcpy(digits, r->cursor + 2, 4);
					const uint32_t low = (uint32_t)strtoul(digits, NULL, 16);
					r->cursor += 6;
					point = 0x10000 + ((point - 0xD800) << 10) + (low - 0xDC00);
				}
				_mc_utf8(&out, point);
				break;
			}
			default: mc_string_append_char(&out, escape); break;
		}
	}
	if ('"' == *r->cursor) ++r->cursor;
	return mc_string_take(&out);
}

static _mc_json_s* _mc_json_value(_mc_json_reader_s* const r) {
	_mc_json_space(r);
	_mc_json_s* const value = mc_calloc(1, sizeof(*value));
	const char c = *r->cursor;
	if ('{' == c || '[' == c) {
		value->kind = ('{' == c) ? _mc_json_kind_object : _mc_json_kind_array;
		const char close = ('{' == c) ? '}' : ']';
		++r->cursor;
		_mc_json_space(r);
		while (NULL == r->error && *r->cursor != close) {
			if (_mc_json_kind_object == value->kind) {
				_mc_json_space(r);
				if ('"' != *r->cursor) {
					r->error = "expected a key";
					break;
				}
				mc_vector_push(&value->keys, _mc_json_text(r));
				_mc_json_space(r);
				if (':' != *r->cursor++) r->error = "expected ':'";
			}
			mc_vector_push(&value->items, _mc_json_value(r));
			_mc_json_space(r);
			if (',' == *r->cursor) ++r->cursor;
			else if (*r->cursor != close) r->error = "expected ',' or a closing bracket";
			_mc_json_space(r);
		}
		if (*r->cursor == close) ++r->cursor;
		return value;
	}
	if ('"' == c) {
		value->kind = _mc_json_kind_string;
		value->text = _mc_json_text(r);
		return value;
	}
	if (0 == strncmp(r->cursor, "null", 4)) { r->cursor += 4; value->kind = _mc_json_kind_null; return value; }
	if (0 == strncmp(r->cursor, "true", 4)) { r->cursor += 4; value->kind = _mc_json_kind_bool; value->boolean = true; return value; }
	if (0 == strncmp(r->cursor, "false", 5)) { r->cursor += 5; value->kind = _mc_json_kind_bool; return value; }
	if (0 == strncmp(r->cursor, "NaN", 3)) { r->cursor += 3; value->kind = _mc_json_kind_real; value->real = NAN; return value; }
	if (0 == strncmp(r->cursor, "Infinity", 8)) { r->cursor += 8; value->kind = _mc_json_kind_real; value->real = INFINITY; return value; }
	if (0 == strncmp(r->cursor, "-Infinity", 9)) { r->cursor += 9; value->kind = _mc_json_kind_real; value->real = -INFINITY; return value; }
	const char* const start = r->cursor;
	if ('-' == *r->cursor) ++r->cursor;
	bool real = false;
	while (isdigit((unsigned char)*r->cursor) || '.' == *r->cursor || 'e' == *r->cursor || 'E' == *r->cursor
			|| (('+' == *r->cursor || '-' == *r->cursor) && ('e' == r->cursor[-1] || 'E' == r->cursor[-1]))) {
		if (!isdigit((unsigned char)*r->cursor)) real = true;
		++r->cursor;
	}
	if (r->cursor == start) {
		r->error = "unexpected character";
		value->kind = _mc_json_kind_null;
		return value;
	}
	if (real) {
		value->kind = _mc_json_kind_real;
		value->real = strtod(start, NULL);
	}
	else {
		value->kind = _mc_json_kind_integer;
		const bool negative = '-' == *start;
		__int128 number = 0;
		for (const char* digit = start + (negative ? 1 : 0); digit < r->cursor; ++digit) number = number * 10 + (*digit - '0');
		value->integer = negative ? -number : number;
	}
	return value;
}

static _mc_json_s* _mc_json_get(const _mc_json_s* const object, const char* const key) {
	if (NULL == object || object->kind != _mc_json_kind_object) return NULL;
	for (size_t i = 0; i < object->keys.count; ++i) if (0 == strcmp(object->keys.data[i], key)) return object->items.data[i];
	return NULL;
}

static int64_t _mc_json_integer(const _mc_json_s* const value) {
	if (NULL == value) return 0;
	if (_mc_json_kind_integer == value->kind) return (int64_t)value->integer;
	if (_mc_json_kind_bool == value->kind) return value->boolean;
	return 0;
}

static mc_bc_constant_s _mc_constant_from_json(const _mc_json_s* const value) {
	mc_bc_constant_s constant = {0};
	switch (value->kind) {
		case _mc_json_kind_null:    constant.kind = mc_constant_none; break;
		case _mc_json_kind_bool:    constant.kind = mc_constant_boolean; constant.boolean = value->boolean; break;
		case _mc_json_kind_integer: constant.kind = mc_constant_integer; constant.integer = value->integer; break;
		case _mc_json_kind_real:    constant.kind = mc_constant_real; constant.real = value->real; break;
		case _mc_json_kind_string:  constant.kind = mc_constant_text; constant.text = mc_strdup(value->text); break;
		case _mc_json_kind_array:
			constant.kind = mc_constant_list;
			for (size_t i = 0; i < value->items.count; ++i) mc_vector_push(&constant.items, _mc_constant_from_json(value->items.data[i]));
			break;
		case _mc_json_kind_object: {
			const _mc_json_s* const slice = _mc_json_get(value, "slice");
			if (slice != NULL) {
				constant.kind = mc_constant_slice;
				for (size_t i = 0; i < slice->items.count; ++i) mc_vector_push(&constant.items, _mc_constant_from_json(slice->items.data[i]));
				constant.start = _mc_json_integer(_mc_json_get(value, "start"));
				constant.length = _mc_json_integer(_mc_json_get(value, "length"));
				const _mc_json_s* const read_only = _mc_json_get(value, "read_only");
				constant.read_only = read_only != NULL && read_only->boolean;
				constant.null = 0 == constant.items.count && 0 == constant.length;
			}
			else if (_mc_json_get(value, "function") != NULL) {
				constant.kind = mc_constant_function;
				constant.integer = _mc_json_integer(_mc_json_get(value, "function"));
			}
			else {
				constant.kind = mc_constant_builtin;
				constant.integer = _mc_json_integer(_mc_json_get(value, "builtin"));
			}
			break;
		}
	}
	return constant;
}

static char* _mc_json_strdup(const _mc_json_s* const value) {
	return mc_strdup((value != NULL && _mc_json_kind_string == value->kind) ? value->text : "");
}

bool mc_module_bc_load(const char* const path, mc_module_bc_s* const module, char** const error) {
	*module = (mc_module_bc_s) {0};
	mc_string_s content = {0};
	if (!mc_read_file(path, &content)) {
		*error = mc_format("cannot read `%s`", path);
		mc_string_free(&content);
		return false;
	}
	const char* const text = mc_string_cstr(&content);
	if (0 != strncmp(text, _mc_magic "\n", strlen(_mc_magic) + 1)) {
		*error = mc_format("`%s` is not an mc bytecode file (version 1)", path);
		mc_string_free(&content);
		return false;
	}
	_mc_json_reader_s reader = { .cursor = text + strlen(_mc_magic) + 1, };
	_mc_json_s* const root = _mc_json_value(&reader);
	bool ok = NULL == reader.error && _mc_json_kind_object == root->kind;
	const _mc_json_s* const functions = _mc_json_get(root, "functions");
	for (size_t f = 0; ok && functions != NULL && f < functions->items.count; ++f) {
		const _mc_json_s* const object = functions->items.data[f];
		mc_bc_function_s function = {0};
		function.name = _mc_json_strdup(_mc_json_get(object, "name"));
		function.parameters = _mc_json_integer(_mc_json_get(object, "parameters"));
		function.locals = _mc_json_integer(_mc_json_get(object, "locals"));
		const _mc_json_s* const returns = _mc_json_get(object, "returns");
		function.returns = returns != NULL && returns->boolean;
		const _mc_json_s* const code = _mc_json_get(object, "code");
		for (size_t i = 0; code != NULL && i < code->items.count; ++i) {
			const _mc_json_s* const entry = code->items.data[i];
			mc_instruction_s instruction = {0};
			if (entry->items.count < 2 || !mc_instruction_parse(entry->items.data[0]->text, &instruction)) {
				ok = false;
				break;
			}
			const _mc_json_s* const operand = entry->items.data[1];
			if (_mc_json_kind_integer == operand->kind) {
				instruction.has_a = true;
				instruction.a = (int64_t)operand->integer;
			}
			else if (_mc_json_kind_array == operand->kind && operand->items.count == 2) {
				instruction.has_a = true;
				instruction.a = _mc_json_integer(operand->items.data[0]);
				instruction.has_b = true;
				instruction.b = _mc_json_integer(operand->items.data[1]);
				instruction.b_bool = _mc_json_kind_bool == operand->items.data[1]->kind;
			}
			mc_vector_push(&function.code, instruction);
		}
		const _mc_json_s* const positions = _mc_json_get(object, "positions");
		for (size_t i = 0; positions != NULL && i < positions->items.count; ++i) {
			const _mc_json_s* const entry = positions->items.data[i];
			mc_position_s position = { .file = -1, };
			if (_mc_json_kind_array == entry->kind && 4 == entry->items.count) {
				position.file = (int32_t)_mc_json_integer(entry->items.data[0]);
				position.line = (uint32_t)_mc_json_integer(entry->items.data[1]);
				position.column = (uint32_t)_mc_json_integer(entry->items.data[2]);
				position.length = (uint32_t)_mc_json_integer(entry->items.data[3]);
			}
			mc_vector_push(&function.positions, position);
		}
		mc_vector_push(&module->functions, function);
	}
	const _mc_json_s* const constants = _mc_json_get(root, "constants");
	for (size_t i = 0; ok && constants != NULL && i < constants->items.count; ++i) mc_vector_push(&module->constants, _mc_constant_from_json(constants->items.data[i]));
	const _mc_json_s* const types = _mc_json_get(root, "types");
	for (size_t i = 0; ok && types != NULL && i < types->items.count; ++i) {
		const _mc_json_s* const object = types->items.data[i];
		mc_bc_type_s type = { .zero = -1, };
		type.name = _mc_json_strdup(_mc_json_get(object, "name"));
		type.kind = _mc_json_strdup(_mc_json_get(object, "kind"));
		type.element = _mc_json_integer(_mc_json_get(object, "element"));
		type.length = _mc_json_integer(_mc_json_get(object, "length"));
		const _mc_json_s* const fields = _mc_json_get(object, "fields");
		for (size_t f = 0; fields != NULL && f < fields->items.count; ++f) {
			mc_vector_push(&type.field_names, _mc_json_strdup(fields->items.data[f]->items.data[0]));
			mc_vector_push(&type.field_types, _mc_json_integer(fields->items.data[f]->items.data[1]));
		}
		const _mc_json_s* const zero = _mc_json_get(object, "zero");
		if (zero != NULL && _mc_json_kind_integer == zero->kind) type.zero = (int64_t)zero->integer;
		mc_vector_push(&module->types, type);
	}
	const _mc_json_s* const builtins = _mc_json_get(root, "builtins");
	for (size_t i = 0; ok && builtins != NULL && i < builtins->items.count; ++i) {
		const _mc_json_s* const entry = builtins->items.data[i];
		mc_bc_builtin_s builtin = {0};
		builtin.name = _mc_json_strdup(entry->items.data[0]);
		for (size_t t = 0; t < entry->items.data[1]->items.count; ++t) mc_vector_push(&builtin.types, _mc_json_integer(entry->items.data[1]->items.data[t]));
		mc_vector_push(&module->builtins, builtin);
	}
	const _mc_json_s* const natives = _mc_json_get(root, "natives");
	for (size_t i = 0; ok && natives != NULL && i < natives->items.count; ++i) mc_vector_push(&module->natives, _mc_json_strdup(natives->items.data[i]));
	const _mc_json_s* const files = _mc_json_get(root, "files");
	for (size_t i = 0; ok && files != NULL && i < files->items.count; ++i) mc_vector_push(&module->files, _mc_json_strdup(files->items.data[i]));
	module->entry = _mc_json_integer(_mc_json_get(root, "entry"));
	const _mc_json_s* const entry_returns = _mc_json_get(root, "entry_returns");
	module->entry_returns = entry_returns != NULL && entry_returns->boolean;
	if (!ok) *error = mc_format("`%s` is not a valid mc bytecode file%s%s", path, reader.error ? ": " : "", reader.error ? reader.error : "");
	_mc_json_free(root);
	mc_string_free(&content);
	if (!ok) mc_module_bc_free(module);
	return ok;
}

// --- the disassembler -----------------------------------------------------------------------

void mc_bc_constant_text(mc_string_s* const out, const mc_bc_constant_s* const constant) {
	switch (constant->kind) {
		case mc_constant_none:     mc_string_append_cstr(out, "None"); return;
		case mc_constant_integer:  mc_format_int128(out, constant->integer); return;
		case mc_constant_real:     mc_format_float_repr(out, constant->real); return;
		case mc_constant_boolean:  mc_string_append_cstr(out, constant->boolean ? "true" : "false"); return;
		case mc_constant_text:     mc_format_repr_text(out, mc_view_cstr(constant->text)); return;
		case mc_constant_function: mc_string_appendf(out, "function %lld", (long long)constant->integer); return;
		case mc_constant_builtin:  mc_string_appendf(out, "builtin %lld", (long long)constant->integer); return;
		case mc_constant_slice: {
			mc_string_append_cstr(out, "slice {");
			for (int64_t i = 0; i < constant->length && constant->start + i < (int64_t)constant->items.count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_bc_constant_text(out, &constant->items.data[constant->start + i]);
			}
			mc_string_append_char(out, '}');
			return;
		}
		case mc_constant_list:
			mc_string_append_char(out, '{');
			for (size_t i = 0; i < constant->items.count; ++i) {
				if (i > 0) mc_string_append_cstr(out, ", ");
				mc_bc_constant_text(out, &constant->items.data[i]);
			}
			mc_string_append_char(out, '}');
			return;
	}
}

void mc_disassemble(const mc_module_bc_s* const module, mc_string_s* const out) {
	for (size_t i = 0; i < module->constants.count; ++i) {
		mc_string_appendf(out, "constant %zu: ", i);
		mc_bc_constant_text(out, &module->constants.data[i]);
		mc_string_append_char(out, '\n');
	}
	if (module->constants.count > 0) mc_string_append_char(out, '\n');
	for (size_t f = 0; f < module->functions.count; ++f) {
		const mc_bc_function_s* const function = &module->functions.data[f];
		mc_string_appendf(out, "function %zu %s: %lld parameter(s), %lld local(s)%s%s\n", f, function->name, (long long)function->parameters,
			(long long)function->locals, function->returns ? ", returns a value" : "", (int64_t)f == module->entry ? "  (entry)" : "");
		for (size_t i = 0; i < function->code.count; ++i) {
			const mc_instruction_s* const instruction = &function->code.data[i];
			mc_string_s text = {0}, comment = {0}, where = {0};
			mc_instruction_name(instruction, &text);
			if (instruction->has_a) {
				if (instruction->has_b) {
					if (instruction->b_bool) mc_string_appendf(&text, " [%lld, %s]", (long long)instruction->a, instruction->b ? "True" : "False");
					else mc_string_appendf(&text, " [%lld, %lld]", (long long)instruction->a, (long long)instruction->b);
				}
				else {
					mc_string_appendf(&text, " %lld", (long long)instruction->a);
				}
			}
			const mc_opcode_e opcode = instruction->opcode;
			if (mc_op_CALL == opcode || mc_op_FUNCTION == opcode) {
				mc_string_append_cstr(&comment, module->functions.data[instruction->a].name);
			}
			else if (mc_op_CALL_BUILTIN == opcode || mc_op_FUNCTION_BUILTIN == opcode) {
				mc_string_appendf(&comment, "builtin %s", module->builtins.data[instruction->a].name);
			}
			else if (mc_op_CALL_NATIVE == opcode) {
				mc_string_appendf(&comment, "extern %s", module->natives.data[instruction->a]);
			}
			else if (mc_op_CONST == opcode || mc_op_ASSERT_FAIL == opcode) {
				mc_bc_constant_text(&comment, &module->constants.data[instruction->a]);
			}
			if (comment.length > 40) {
				comment.length = 37;
				comment.data[37] = '\0';
				mc_string_append_cstr(&comment, "...");
			}
			const mc_position_s* const position = (i < function->positions.count) ? &function->positions.data[i] : NULL;
			if (position != NULL && position->file >= 0) {
				const char* const file = module->files.data[position->file];
				const char* const slash = strrchr(file, '/');
				const char* const backslash = strrchr(file, '\\');
				const char* base = file;
				if (slash != NULL && slash + 1 > base) base = slash + 1;
				if (backslash != NULL && backslash + 1 > base) base = backslash + 1;
				mc_string_appendf(&where, "%s:%u", base, position->line);
			}
			mc_string_s line = {0};
			mc_string_appendf(&line, "  %4zu  %-28s %-16s %s", i, mc_string_cstr(&text), mc_string_cstr(&where), mc_string_cstr(&comment));
			while (line.length > 0 && ' ' == line.data[line.length - 1]) line.data[--line.length] = '\0';
			mc_string_append(out, mc_string_view(&line));
			mc_string_append_char(out, '\n');
			mc_string_free(&line);
			mc_string_free(&text);
			mc_string_free(&comment);
			mc_string_free(&where);
		}
		mc_string_append_char(out, '\n');
	}
}
