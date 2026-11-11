#ifndef CRSTRUCTBASE_H
#define CRSTRUCTBASE_H

// The checks every call makes of a caller's based struct (CR_STRUCT_BASE,
// crcore.h) before it reads any other field, written once so that every
// module and backend returns the same result for the same struct.
//
// These two are for a struct nothing can be chained onto, so anything in
// its chain is a struct this build doesn't take there. A call that takes
// chained structs walks its own chain instead, as
// crgpu_adapter_get_properties() does (src/gpu/crgpushared.c).

#include "crcore.h"

// A struct the caller filled in: [type] is the type the call takes there.
static inline CR_RESULT cr_struct_check(const CR_STRUCT_BASE *base, CR_STRUCT_TYPE type) {
	if (base->type != type)
		return CR_RESULT_STRUCT_WRONG_TYPE;
	if (base->next)
		return CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN;
	return CR_SUCCESS;
}

// A struct the caller hands over for Carrottus to fill in: the same checks,
// made before anything is written to it.
static inline CR_RESULT cr_struct_out_check(const CR_STRUCT_BASE_OUT *base, CR_STRUCT_TYPE type) {
	if (base->type != type)
		return CR_RESULT_STRUCT_WRONG_TYPE;
	if (base->next)
		return CR_RESULT_STRUCT_UNKNOWN_IN_CHAIN;
	return CR_SUCCESS;
}

// The base a struct Carrottus hands back has: its own type, and no chain.
static inline CR_STRUCT_BASE cr_struct_base(CR_STRUCT_TYPE type) {
	CR_STRUCT_BASE base;
	base.type = type;
	base.next = NULL;
	return base;
}

static inline CR_STRUCT_BASE_OUT cr_struct_base_out(CR_STRUCT_TYPE type) {
	CR_STRUCT_BASE_OUT base;
	base.type = type;
	base.next = NULL;
	return base;
}

#endif // CRSTRUCTBASE_H
