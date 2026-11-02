/*
 * spudgpu_msl: SPIR-V in, Metal source out, in the binding layout SpudGPU's
 * Metal backend reads (spudgpu.h, spudgpu_shader_module_desc). A build tool
 * for the caller's shader build, run on the build machine; never part of
 * the library, which compiles nothing (card A9, revised). It carries the
 * SPIRV-Cross options and remaps the Metal backend applied at run time
 * until 2026-10-31 (card S20), for card A29's fallback route: Slang to
 * SPIR-V, then this, then Apple's compiler.
 *
 *     spudgpu_msl --platform macos|ios --msl-version <MMmmpp>
 *                 --reflection <reflection.json> <in.spv> <out.metal>
 *
 * Every choice is the command line's: no platform and no Metal language
 * version is assumed. The module's one entry point keeps its name (main
 * excepted, which Metal reserves and SPIRV-Cross renames main0). Exits 0,
 * or 1 with the reason on stderr.
 *
 * Every binding of a set (card S27, spudgpu_shader_module_desc): a compiler
 * may leave out of the SPIR-V the bindings its entry point doesn't read, and
 * Metal lays out a set's argument buffer from the members a module declares.
 * <reflection.json> is Slang's reflection of the same source
 * (-reflection-json), which lists every binding the source declares, read
 * or not; each set the module reads gets a member for every binding listed
 * in it that SPIRV-Cross didn't write, of the binding's Metal kind
 * (buffer, texture, sampler, acceleration structure) and count, at the
 * binding's [[id]]. A set the module reads none of is left out, as before.
 * SPIRV-Cross's own padding (pad_argument_buffer_resources) can't do this:
 * it walks argument buffer ids from 0 up and throws at a gap, which every
 * layout with a combined image sampler (its sampler at 1024 + the binding)
 * and any layout of sparse bindings has, and it adds nothing past the last
 * member the module declares.
 */

#include <spirv_cross_c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The layout spudgpu.h states for Metal: set s's argument buffer at buffer
 * index 26 + s (SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS sets below the push
 * constants), the push constants at buffer index 30, and a combined image
 * sampler's sampler at [[id(1024 + binding)]]. */
#define SPUDGPU_MSL_MAX_SETS 4
#define SPUDGPU_MSL_PUSH_CONSTANTS_BUFFER 30
#define SPUDGPU_MSL_SET_BUFFER_BASE (SPUDGPU_MSL_PUSH_CONSTANTS_BUFFER - SPUDGPU_MSL_MAX_SETS)
#define SPUDGPU_MSL_COMBINED_SAMPLER_BASE 1024u

/* What the tool reads: a reflection file, nested this deep at most, and a
 * set's members, this many at most (SpudGPU's bindings per set, 32, each a
 * combined image sampler's two). Past either, the tool says so and stops. */
#define SPUDGPU_MSL_MAX_JSON_DEPTH 64
#define SPUDGPU_MSL_MAX_SET_MEMBERS 64

static int fail(const char *what, const char *detail) {
	fprintf(stderr, "spudgpu_msl: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	return 1;
}

/* A whole file, read into memory with a 0 after it. */
static char *read_file(const char *path, size_t *out_size) {
	FILE *in = fopen(path, "rb");
	if (!in)
		return NULL;
	if (fseek(in, 0, SEEK_END) != 0) {
		fclose(in);
		return NULL;
	}
	const long size = ftell(in);
	if (size < 0 || fseek(in, 0, SEEK_SET) != 0) {
		fclose(in);
		return NULL;
	}
	char *data = (char *)malloc((size_t)size + 1);
	if (!data) {
		fclose(in);
		return NULL;
	}
	const size_t read = fread(data, 1, (size_t)size, in);
	fclose(in);
	if (read != (size_t)size) {
		free(data);
		return NULL;
	}
	data[size] = 0;
	*out_size  = (size_t)size;
	return data;
}

/* ----------------------------------------------------------------------
 * JSON, as much as reading Slang's reflection needs: a tree of nodes over
 * the file's bytes, every value kept as its span. Strings are compared as
 * they are written (the keys and values read here have no escapes).
 * ---------------------------------------------------------------------- */

typedef enum json_kind { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_kind;

typedef struct json_node {
	json_kind kind;
	/* The value's span; for a string, its bytes between the quotes. */
	size_t start, length;
	/* For a member of an object, its key's span between the quotes. */
	size_t key_start, key_length;
	/* Children of an array or object, then the next sibling: indices into
	 * the node array, 0 for none (node 0 is the root, nobody's child). */
	uint32_t first_child, next_sibling;
} json_node;

typedef struct json_doc {
	const char *text;
	size_t size;
	json_node *nodes;
	uint32_t count, capacity;
	const char *error;
} json_doc;

static void json_skip_space(const json_doc *doc, size_t *at) {
	while (*at < doc->size && (doc->text[*at] == ' ' || doc->text[*at] == '\t' || doc->text[*at] == '\n' || doc->text[*at] == '\r'))
		++*at;
}

/* The string starting at doc->text[*at] (a quote): its span between the
 * quotes, *at moved past the closing one. */
static int json_string_span(json_doc *doc, size_t *at, size_t *out_start, size_t *out_length) {
	if (*at >= doc->size || doc->text[*at] != '"') {
		doc->error = "a string was expected";
		return 0;
	}
	const size_t start = ++*at;
	while (*at < doc->size && doc->text[*at] != '"') {
		if (doc->text[*at] == '\\') {
			if (*at + 1 >= doc->size) {
				doc->error = "a string ends in an escape";
				return 0;
			}
			++*at;
		}
		++*at;
	}
	if (*at >= doc->size) {
		doc->error = "a string isn't closed";
		return 0;
	}
	*out_start  = start;
	*out_length = *at - start;
	++*at;
	return 1;
}

static int json_literal(json_doc *doc, size_t *at, const char *literal) {
	const size_t length = strlen(literal);
	if (doc->size - *at < length || memcmp(doc->text + *at, literal, length) != 0) {
		doc->error = "an unknown value";
		return 0;
	}
	*at += length;
	return 1;
}

/* Parses the value at *at into a new node, returning its index, or 0 with
 * doc->error set (node 0 is only ever the root). */
static uint32_t json_value(json_doc *doc, size_t *at, unsigned depth) {
	if (depth > SPUDGPU_MSL_MAX_JSON_DEPTH) {
		doc->error = "nested too deeply";
		return 0;
	}
	json_skip_space(doc, at);
	if (*at >= doc->size) {
		doc->error = "a value was expected";
		return 0;
	}
	if (doc->count == doc->capacity) {
		doc->error = "more values than bytes";
		return 0;
	}
	const uint32_t index = doc->count++;
	json_node *node      = &doc->nodes[index];
	memset(node, 0, sizeof(*node));
	node->start    = *at;
	const char c   = doc->text[*at];
	if (c == '{' || c == '[') {
		const int object = c == '{';
		node->kind       = object ? JSON_OBJECT : JSON_ARRAY;
		++*at;
		uint32_t last = 0;
		json_skip_space(doc, at);
		if (*at < doc->size && doc->text[*at] == (object ? '}' : ']')) {
			++*at;
		} else {
			for (;;) {
				size_t key_start = 0, key_length = 0;
				if (object) {
					json_skip_space(doc, at);
					if (!json_string_span(doc, at, &key_start, &key_length))
						return 0;
					json_skip_space(doc, at);
					if (*at >= doc->size || doc->text[*at] != ':') {
						doc->error = "a ':' was expected";
						return 0;
					}
					++*at;
				}
				const uint32_t child = json_value(doc, at, depth + 1);
				if (!child)
					return 0;
				doc->nodes[child].key_start  = key_start;
				doc->nodes[child].key_length = key_length;
				if (last)
					doc->nodes[last].next_sibling = child;
				else
					doc->nodes[index].first_child = child;
				last = child;
				json_skip_space(doc, at);
				if (*at < doc->size && doc->text[*at] == ',') {
					++*at;
					continue;
				}
				if (*at < doc->size && doc->text[*at] == (object ? '}' : ']')) {
					++*at;
					break;
				}
				doc->error = object ? "a ',' or '}' was expected" : "a ',' or ']' was expected";
				return 0;
			}
		}
		node = &doc->nodes[index];
	} else if (c == '"') {
		node->kind = JSON_STRING;
		if (!json_string_span(doc, at, &node->start, &node->length))
			return 0;
		return index;
	} else if (c == 't' || c == 'f') {
		node->kind = JSON_BOOL;
		if (!json_literal(doc, at, c == 't' ? "true" : "false"))
			return 0;
	} else if (c == 'n') {
		node->kind = JSON_NULL;
		if (!json_literal(doc, at, "null"))
			return 0;
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		node->kind = JSON_NUMBER;
		while (*at < doc->size && strchr("-+.eE0123456789", doc->text[*at]))
			++*at;
	} else {
		doc->error = "an unknown value";
		return 0;
	}
	node->length = *at - node->start;
	return index;
}

static int json_equals(const json_doc *doc, size_t start, size_t length, const char *literal) {
	return strlen(literal) == length && memcmp(doc->text + start, literal, length) == 0;
}

/* An object's member by key, or NULL. */
static const json_node *json_member(const json_doc *doc, const json_node *object, const char *key) {
	if (!object || object->kind != JSON_OBJECT)
		return NULL;
	for (uint32_t i = object->first_child; i; i = doc->nodes[i].next_sibling)
		if (json_equals(doc, doc->nodes[i].key_start, doc->nodes[i].key_length, key))
			return &doc->nodes[i];
	return NULL;
}

static int json_is_string(const json_doc *doc, const json_node *node, const char *value) {
	return node && node->kind == JSON_STRING && json_equals(doc, node->start, node->length, value);
}

/* A non-negative integer below 2^31, or -1. */
static long json_uint(const json_doc *doc, const json_node *node) {
	if (!node || node->kind != JSON_NUMBER || node->length == 0 || node->length > 10)
		return -1;
	long value = 0;
	for (size_t i = 0; i < node->length; ++i) {
		const char c = doc->text[node->start + i];
		if (c < '0' || c > '9')
			return -1;
		value = value * 10 + (c - '0');
	}
	return value <= 0x7fffffffL ? value : -1;
}

/* ----------------------------------------------------------------------
 * The bindings the reflection lists, as the Metal kinds a set's argument
 * buffer holds.
 * ---------------------------------------------------------------------- */

typedef enum msl_kind { MSL_CONSTANT_BUFFER, MSL_DEVICE_BUFFER, MSL_TEXTURE, MSL_STORAGE_TEXTURE, MSL_SAMPLER, MSL_ACCELERATION_STRUCTURE } msl_kind;

typedef struct msl_binding {
	uint32_t set, binding, count;
	msl_kind kind;
	/* For a texture: Metal's texture type, "texture2d"... */
	const char *texture;
	/* A combined image sampler: a texture at the binding's id and its
	 * sampler at SPUDGPU_MSL_COMBINED_SAMPLER_BASE + the binding. */
	int combined;
} msl_binding;

static const char *msl_texture_type(const json_doc *doc, const json_node *shape) {
	static const char *const shapes[][2] = {
	    {"texture1D", "texture1d"},
	    {"texture1DArray", "texture1d_array"},
	    {"texture2D", "texture2d"},
	    {"texture2DArray", "texture2d_array"},
	    {"texture2DMultisample", "texture2d_ms"},
	    {"texture2DMultisampleArray", "texture2d_ms_array"},
	    {"texture3D", "texture3d"},
	    {"textureCube", "texturecube"},
	    {"textureCubeArray", "texturecube_array"},
	};
	for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i)
		if (json_is_string(doc, shape, shapes[i][0]))
			return shapes[i][1];
	return NULL;
}

/* Reads one parameter of the reflection into *out. Returns 1 for a binding
 * of a descriptor set, 0 for a parameter that is none (the push constants),
 * -1 for a shape this tool doesn't know, with *out_why set. */
static int read_binding(const json_doc *doc, const json_node *parameter, msl_binding *out, const char **out_why) {
	const json_node *binding = json_member(doc, parameter, "binding");
	if (!binding) {
		const json_node *bindings = json_member(doc, parameter, "bindings");
		if (bindings && bindings->kind == JSON_ARRAY)
			for (uint32_t i = bindings->first_child; i; i = doc->nodes[i].next_sibling)
				if (json_is_string(doc, json_member(doc, &doc->nodes[i], "kind"), "pushConstantBuffer"))
					return 0;
		*out_why = "a parameter with several bindings that isn't the push constants";
		return -1;
	}
	if (!json_is_string(doc, json_member(doc, binding, "kind"), "descriptorTableSlot")) {
		*out_why = "a binding that isn't a descriptor (kind isn't descriptorTableSlot)";
		return -1;
	}
	const json_node *space = json_member(doc, binding, "space");
	const long set         = space ? json_uint(doc, space) : 0;
	const long index       = json_uint(doc, json_member(doc, binding, "index"));
	if (set < 0 || set >= SPUDGPU_MSL_MAX_SETS || index < 0 || index >= (long)SPUDGPU_MSL_COMBINED_SAMPLER_BASE) {
		*out_why = "a binding's set or index out of SpudGPU's range";
		return -1;
	}
	memset(out, 0, sizeof(*out));
	out->set     = (uint32_t)set;
	out->binding = (uint32_t)index;
	out->count   = 1;

	const json_node *type = json_member(doc, parameter, "type");
	if (json_is_string(doc, json_member(doc, type, "kind"), "array")) {
		const long count = json_uint(doc, json_member(doc, type, "elementCount"));
		if (count < 1) {
			*out_why = "an array of unknown size (bindless)";
			return -1;
		}
		out->count = (uint32_t)count;
		type       = json_member(doc, type, "elementType");
	}
	const json_node *kind = json_member(doc, type, "kind");
	if (json_is_string(doc, kind, "constantBuffer")) {
		out->kind = MSL_CONSTANT_BUFFER;
	} else if (json_is_string(doc, kind, "samplerState")) {
		out->kind = MSL_SAMPLER;
	} else if (json_is_string(doc, kind, "resource")) {
		const json_node *shape = json_member(doc, type, "baseShape");
		const int read_write   = json_is_string(doc, json_member(doc, type, "access"), "readWrite");
		const json_node *combined = json_member(doc, type, "combined");
		if (json_is_string(doc, shape, "structuredBuffer") || json_is_string(doc, shape, "byteAddressBuffer")) {
			out->kind = MSL_DEVICE_BUFFER;
		} else if (json_is_string(doc, shape, "accelerationStructure")) {
			if (out->count != 1) {
				*out_why = "an array of acceleration structures";
				return -1;
			}
			out->kind = MSL_ACCELERATION_STRUCTURE;
		} else if ((out->texture = msl_texture_type(doc, shape)) != NULL) {
			out->kind     = read_write ? MSL_STORAGE_TEXTURE : MSL_TEXTURE;
			out->combined = combined && combined->kind == JSON_BOOL && json_equals(doc, combined->start, combined->length, "true");
		} else {
			*out_why = "a resource of a shape this tool doesn't know";
			return -1;
		}
	} else {
		*out_why = "a parameter of a kind this tool doesn't know";
		return -1;
	}
	return 1;
}

/* The members of one set's argument buffer: each an [[id]] and, for one
 * this tool adds, the binding it stands for. */
typedef struct set_member {
	uint32_t id;
	/* The member's line in SPIRV-Cross's output (start, length), or a
	 * binding of the reflection's to write a member for. */
	size_t line_start, line_length;
	const msl_binding *pad;
	int pad_sampler;
} set_member;

static void append(char **buffer, size_t *length, size_t *capacity, const char *text, size_t text_length, int *failed) {
	if (*failed)
		return;
	if (*capacity - *length < text_length + 1) {
		size_t grown = *capacity ? *capacity : 4096;
		while (grown - *length < text_length + 1)
			grown *= 2;
		char *bigger = (char *)realloc(*buffer, grown);
		if (!bigger) {
			*failed = 1;
			return;
		}
		*buffer   = bigger;
		*capacity = grown;
	}
	memcpy(*buffer + *length, text, text_length);
	*length += text_length;
	(*buffer)[*length] = 0;
}

/* One padding member's declaration, for [binding] (its sampler when
 * [sampler] is set), into [line]. */
static void pad_line(char *line, size_t size, const msl_binding *binding, int sampler) {
	const uint32_t id = sampler ? SPUDGPU_MSL_COMBINED_SAMPLER_BASE + binding->binding : binding->binding;
	char type[96];
	const char *element = NULL;
	if (sampler)
		element = "sampler";
	switch (sampler ? MSL_SAMPLER : binding->kind) {
	case MSL_CONSTANT_BUFFER:
		element = "constant uchar*";
		break;
	case MSL_DEVICE_BUFFER:
		element = "device uchar*";
		break;
	case MSL_TEXTURE:
		snprintf(type, sizeof(type), "%s<float>", binding->texture);
		element = type;
		break;
	case MSL_STORAGE_TEXTURE:
		snprintf(type, sizeof(type), "%s<float, access::read_write>", binding->texture);
		element = type;
		break;
	case MSL_SAMPLER:
		element = "sampler";
		break;
	case MSL_ACCELERATION_STRUCTURE:
		element = "raytracing::acceleration_structure<raytracing::instancing>";
		break;
	}
	const int buffer = !sampler && (binding->kind == MSL_CONSTANT_BUFFER || binding->kind == MSL_DEVICE_BUFFER);
	// Metal takes an array of textures or samplers as array<>, and an
	// array of buffers as a C array of pointers, as SPIRV-Cross writes them.
	if (binding->count > 1 && !buffer)
		snprintf(line, size, "    array<%s, %u> spud_pad_%u [[id(%u)]];\n", element, binding->count, id, id);
	else if (binding->count > 1)
		snprintf(line, size, "    %s spud_pad_%u [[id(%u)]][%u];\n", element, id, id, binding->count);
	else
		snprintf(line, size, "    %s spud_pad_%u [[id(%u)]];\n", element, id, id);
}

/* Writes [source] to [out_source] with every set it declares padded to
 * the bindings [bindings] lists for it. Returns 0, or 1 with the reason on
 * stderr. */
static int pad_sets(const char *source, const msl_binding *bindings, size_t binding_count, char **out_source) {
	char *padded    = NULL;
	size_t length   = 0;
	size_t capacity = 0;
	int failed      = 0;
	size_t at       = 0;
	static const char prefix[] = "struct spvDescriptorSetBuffer";
	for (const char *found; (found = strstr(source + at, prefix)) != NULL;) {
		char *number_end        = NULL;
		const unsigned long set = strtoul(found + sizeof(prefix) - 1, &number_end, 10);
		if (number_end == found + sizeof(prefix) - 1 || set >= SPUDGPU_MSL_MAX_SETS || strncmp(number_end, "\n{\n", 3) != 0) {
			free(padded);
			return fail("an argument buffer struct this tool can't read", NULL);
		}
		char header[48];
		snprintf(header, sizeof(header), "spvDescriptorSetBuffer%lu", set);
		const size_t body = (size_t)(number_end - source) + 3;
		const char *end   = strstr(source + body, "};\n");
		if (!end) {
			free(padded);
			return fail("an argument buffer struct isn't closed", header);
		}
		const size_t body_end = (size_t)(end - source);

		set_member members[SPUDGPU_MSL_MAX_SET_MEMBERS];
		size_t member_count = 0;
		for (size_t line = body; line < body_end;) {
			const char *newline   = memchr(source + line, '\n', body_end - line);
			const size_t line_end = newline ? (size_t)(newline - source) + 1 : body_end;
			const char *id        = strstr(source + line, "[[id(");
			if (!id || (size_t)(id - source) >= line_end) {
				free(padded);
				return fail("an argument buffer member without an [[id]]", header);
			}
			if (member_count == SPUDGPU_MSL_MAX_SET_MEMBERS) {
				free(padded);
				return fail("more argument buffer members than a set holds", header);
			}
			members[member_count].id          = (uint32_t)strtoul(id + 5, NULL, 10);
			members[member_count].line_start  = line;
			members[member_count].line_length = line_end - line;
			members[member_count].pad         = NULL;
			members[member_count].pad_sampler = 0;
			++member_count;
			line = line_end;
		}
		// Every binding of the set the reflection lists, written where
		// SPIRV-Cross wrote no member at its id.
		for (size_t b = 0; b < binding_count; ++b) {
			if (bindings[b].set != set)
				continue;
			for (int sampler = 0; sampler <= bindings[b].combined; ++sampler) {
				const uint32_t id = sampler ? SPUDGPU_MSL_COMBINED_SAMPLER_BASE + bindings[b].binding : bindings[b].binding;
				int declared      = 0;
				for (size_t m = 0; m < member_count; ++m)
					if (members[m].id == id)
						declared = 1;
				if (declared)
					continue;
				if (member_count == SPUDGPU_MSL_MAX_SET_MEMBERS) {
					free(padded);
					return fail("more argument buffer members than a set holds", header);
				}
				members[member_count].id          = id;
				members[member_count].line_start  = 0;
				members[member_count].line_length = 0;
				members[member_count].pad         = &bindings[b];
				members[member_count].pad_sampler = sampler;
				++member_count;
			}
		}
		// In id order, as SPIRV-Cross writes them and Metal lays them out.
		for (size_t i = 1; i < member_count; ++i)
			for (size_t j = i; j > 0 && members[j - 1].id > members[j].id; --j) {
				const set_member swap = members[j];
				members[j]            = members[j - 1];
				members[j - 1]        = swap;
			}

		append(&padded, &length, &capacity, source + at, body - at, &failed);
		for (size_t m = 0; m < member_count; ++m) {
			if (members[m].pad) {
				char line[192];
				pad_line(line, sizeof(line), members[m].pad, members[m].pad_sampler);
				append(&padded, &length, &capacity, line, strlen(line), &failed);
			} else {
				append(&padded, &length, &capacity, source + members[m].line_start, members[m].line_length, &failed);
			}
		}
		at = body_end;
	}
	append(&padded, &length, &capacity, source + at, strlen(source + at), &failed);
	if (failed) {
		free(padded);
		return fail("out of memory", NULL);
	}
	*out_source = padded;
	return 0;
}

int main(int argc, char **argv) {
	const char *platform        = NULL;
	const char *msl_version     = NULL;
	const char *reflection_path = NULL;
	const char *in_path         = NULL;
	const char *out_path        = NULL;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc)
			platform = argv[++i];
		else if (strcmp(argv[i], "--msl-version") == 0 && i + 1 < argc)
			msl_version = argv[++i];
		else if (strcmp(argv[i], "--reflection") == 0 && i + 1 < argc)
			reflection_path = argv[++i];
		else if (!in_path)
			in_path = argv[i];
		else if (!out_path)
			out_path = argv[i];
		else
			return fail("unexpected argument", argv[i]);
	}
	if (!platform || !msl_version || !reflection_path || !in_path || !out_path)
		return fail("usage: spudgpu_msl --platform macos|ios --msl-version <MMmmpp> --reflection <reflection.json> <in.spv> <out.metal>",
		            NULL);
	unsigned msl_platform;
	if (strcmp(platform, "macos") == 0)
		msl_platform = SPVC_MSL_PLATFORM_MACOS;
	else if (strcmp(platform, "ios") == 0)
		msl_platform = SPVC_MSL_PLATFORM_IOS;
	else
		return fail("--platform is neither macos nor ios", platform);
	char *version_end      = NULL;
	unsigned long version  = strtoul(msl_version, &version_end, 10);
	if (!version_end || *version_end || version == 0)
		return fail("--msl-version is not a number such as 30100", msl_version);

	// The reflection's bindings, before anything else is read.
	size_t json_size = 0;
	char *json_text  = read_file(reflection_path, &json_size);
	if (!json_text)
		return fail("can't read", reflection_path);
	json_doc doc;
	memset(&doc, 0, sizeof(doc));
	doc.text     = json_text;
	doc.size     = json_size;
	doc.capacity = json_size > 0xfffffffeu ? 0xfffffffeu : (uint32_t)json_size + 1;
	doc.nodes    = (json_node *)malloc(sizeof(json_node) * (size_t)doc.capacity);
	if (!doc.nodes) {
		free(json_text);
		return fail("out of memory", NULL);
	}
	size_t parse_at = 0;
	json_value(&doc, &parse_at, 0); // the root, node 0; doc.error says whether it failed
	if (!doc.error) {
		json_skip_space(&doc, &parse_at);
		if (parse_at != doc.size)
			doc.error = "text after the reflection's object";
	}
	const json_node *parameters = doc.error ? NULL : json_member(&doc, &doc.nodes[0], "parameters");
	if (!doc.error && (!parameters || parameters->kind != JSON_ARRAY))
		doc.error = "no parameters array";
	if (doc.error) {
		fail(doc.error, reflection_path);
		free(doc.nodes);
		free(json_text);
		return 1;
	}
	size_t binding_count = 0;
	for (uint32_t i = parameters->first_child; i; i = doc.nodes[i].next_sibling)
		++binding_count;
	msl_binding *bindings = (msl_binding *)calloc(binding_count ? binding_count : 1, sizeof(msl_binding));
	if (!bindings) {
		free(doc.nodes);
		free(json_text);
		return fail("out of memory", NULL);
	}
	binding_count = 0;
	for (uint32_t i = parameters->first_child; i; i = doc.nodes[i].next_sibling) {
		const char *why = NULL;
		const int read  = read_binding(&doc, &doc.nodes[i], &bindings[binding_count], &why);
		if (read < 0) {
			fail(why, reflection_path);
			free(bindings);
			free(doc.nodes);
			free(json_text);
			return 1;
		}
		binding_count += (size_t)read;
	}
	free(doc.nodes);
	free(json_text);

	size_t size = 0;
	char *bytes = read_file(in_path, &size);
	if (!bytes) {
		free(bindings);
		return fail("can't read", in_path);
	}
	if (size == 0 || size % 4 != 0) {
		free(bytes);
		free(bindings);
		return fail("not SPIR-V words", in_path);
	}
	SpvId *words = (SpvId *)malloc(size);
	if (!words) {
		free(bytes);
		free(bindings);
		return fail("out of memory", NULL);
	}
	memcpy(words, bytes, size);
	free(bytes);

	spvc_context context = NULL;
	if (spvc_context_create(&context) != SPVC_SUCCESS) {
		free(words);
		free(bindings);
		return fail("SPIRV-Cross context", NULL);
	}
	int status               = 1;
	spvc_parsed_ir parsed_ir = NULL;
	spvc_compiler compiler   = NULL;
	char *padded             = NULL;
	if (spvc_context_parse_spirv(context, words, size / sizeof(SpvId), &parsed_ir) != SPVC_SUCCESS) {
		fail("SPIR-V parse failed", spvc_context_get_last_error_string(context));
		goto done;
	}
	if (spvc_context_create_compiler(context, SPVC_BACKEND_MSL, parsed_ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) !=
	    SPVC_SUCCESS) {
		fail("SPIRV-Cross compiler", spvc_context_get_last_error_string(context));
		goto done;
	}

	const spvc_entry_point *entry_points = NULL;
	size_t entry_point_count             = 0;
	spvc_compiler_get_entry_points(compiler, &entry_points, &entry_point_count);
	if (entry_point_count != 1) {
		fail("a module has one entry point (spudgpu.h)", NULL);
		goto done;
	}
	const SpvExecutionModel model = entry_points[0].execution_model;

	spvc_compiler_options options = NULL;
	spvc_compiler_create_compiler_options(compiler, &options);
	spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_VERSION, (unsigned)version);
	spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_PLATFORM, msl_platform);
	// SPIRV-Cross refuses a vertex shader's base vertex or instance on iOS
	// unless told the GPU has them (A9 and later). Every GPU that runs Metal
	// 3 is an A13 or later, so from that version it is a fact, not a choice.
	if (msl_platform == SPVC_MSL_PLATFORM_IOS && version >= 30000)
		spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_IOS_SUPPORT_BASE_VERTEX_INSTANCE, SPVC_TRUE);
	// Every set is an argument buffer (tier 1), each member at its binding's
	// own [[id]], every resource the SPIR-V declares kept in it whether the
	// entry point reads it or not; what the SPIR-V left out is padded after
	// (pad_sets).
	spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_ARGUMENT_BUFFERS, SPVC_TRUE);
	spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_ARGUMENT_BUFFERS_TIER, 1u);
	spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_ENABLE_DECORATION_BINDING, SPVC_TRUE);
	spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_FORCE_ACTIVE_ARGUMENT_BUFFER_RESOURCES, SPVC_TRUE);
	// No Y flip: clip space is +Y up on every backend (card S19).
	spvc_compiler_install_compiler_options(compiler, options);

	for (unsigned set = 0; set < SPUDGPU_MSL_MAX_SETS; ++set) {
		spvc_msl_resource_binding_2 binding;
		spvc_msl_resource_binding_init_2(&binding);
		binding.stage      = model;
		binding.desc_set   = set;
		binding.binding    = SPVC_MSL_ARGUMENT_BUFFER_BINDING;
		binding.msl_buffer = SPUDGPU_MSL_SET_BUFFER_BASE + set;
		spvc_compiler_msl_add_resource_binding_2(compiler, &binding);
	}
	{
		spvc_resources resources                      = NULL;
		const spvc_reflected_resource *sampled_images = NULL;
		size_t sampled_image_count                    = 0;
		if (spvc_compiler_create_shader_resources(compiler, &resources) == SPVC_SUCCESS &&
		    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &sampled_images, &sampled_image_count) ==
		        SPVC_SUCCESS) {
			for (size_t i = 0; i < sampled_image_count; ++i) {
				const unsigned set     = spvc_compiler_get_decoration(compiler, sampled_images[i].id, SpvDecorationDescriptorSet);
				const unsigned binding = spvc_compiler_get_decoration(compiler, sampled_images[i].id, SpvDecorationBinding);
				spvc_msl_resource_binding_2 combined;
				spvc_msl_resource_binding_init_2(&combined);
				combined.stage       = model;
				combined.desc_set    = set;
				combined.binding     = binding;
				combined.msl_texture = binding;
				combined.msl_sampler = SPUDGPU_MSL_COMBINED_SAMPLER_BASE + binding;
				spvc_compiler_msl_add_resource_binding_2(compiler, &combined);
			}
		}
	}
	{
		spvc_msl_resource_binding_2 push_constants;
		spvc_msl_resource_binding_init_2(&push_constants);
		push_constants.stage      = model;
		push_constants.desc_set   = SPVC_MSL_PUSH_CONSTANT_DESC_SET;
		push_constants.binding    = SPVC_MSL_PUSH_CONSTANT_BINDING;
		push_constants.msl_buffer = SPUDGPU_MSL_PUSH_CONSTANTS_BUFFER;
		spvc_compiler_msl_add_resource_binding_2(compiler, &push_constants);
	}

	const char *source = NULL;
	if (spvc_compiler_compile(compiler, &source) != SPVC_SUCCESS || !source) {
		fail("SPIR-V to Metal failed", spvc_context_get_last_error_string(context));
		goto done;
	}
	if (pad_sets(source, bindings, binding_count, &padded) != 0)
		goto done;
	FILE *out = fopen(out_path, "wb");
	if (!out) {
		fail("can't write", out_path);
		goto done;
	}
	const size_t length = strlen(padded);
	status              = fwrite(padded, 1, length, out) == length ? 0 : fail("can't write", out_path);
	if (fclose(out) != 0)
		status = fail("can't write", out_path);

done:
	spvc_context_destroy(context);
	free(padded);
	free(words);
	free(bindings);
	return status;
}
