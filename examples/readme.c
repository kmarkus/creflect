#include <errno.h>
#include <stdio.h>
#include "crefl.h"

/* the types, as C source: a string here, or a header read from a file */
static const char hdr[] =
	"enum mode { MODE_OFF, MODE_POS, MODE_VEL };\n"
	"typedef struct {\n"
	"	char name[16];\n"
	"	double gain;\n"
	"	enum mode mode;\n"
	"	struct { uint16_t lo, hi; } range[NRANGE];\n"
	"	union { uint32_t raw; struct { unsigned en : 1, err : 3; }; };\n"
	"} __attribute__((packed)) cfg_t;\n";

int main(void)
{
	/* the context owns all types, crefl_ctx_new_static() avoids malloc */
	crefl_ctx_t *ctx = crefl_ctx_new();

	/* macros are not expanded, so define the ones the types use */
	crefl_define(ctx, "NRANGE", 2);
	if (crefl_parse(ctx, hdr)) {
		fprintf(stderr, "%s\n", crefl_errmsg(ctx)); /* "line 2: ..." */
		return 1;
	}

	/* look up types once, at init */
	const crefl_type_t *t = crefl_type(ctx, "cfg_t");

	/* a new zeroed instance, owned by v */
	crefl_val_t v = crefl_new(t);
	double gain;
	int64_t mode;

	/* members are addressed by C designators, setters check ranges */
	crefl_set_str(crefl_get(v, "name"), "motor1");
	crefl_set_f64(crefl_get(v, "gain"), 0.5);
	crefl_set_i64(crefl_get(v, "mode"), 2); /* MODE_VEL */
	crefl_set_u64(crefl_get(v, "range[1].hi"), 1000);
	crefl_set_u64(crefl_get(v, "en"), 1); /* anonymous members */
	if (crefl_set_u64(crefl_get(v, "err"), 8) == -ERANGE) /* 3 bits */
		printf("err: out of range\n");

	crefl_get_f64(crefl_get(v, "gain"), &gain);
	crefl_get_i64(crefl_get(v, "mode"), &mode);
	printf("gain=%g mode=%lld\n", gain, (long long)mode);

	/* arrays: copy range[1] to range[0] */
	crefl_val_t r = crefl_get(v, "range");
	crefl_copy(crefl_index(r, 0), crefl_index(r, 1));

	/* hot path: resolve the path once, then access in O(1) */
	crefl_path_t p;
	uint64_t raw;
	crefl_path(t, "raw", &p);
	crefl_get_u64(crefl_at(v, &p), &raw);
	printf("raw=0x%llx\n", (unsigned long long)raw);

	/*
	 * attach to existing memory, e.g. a received packet or a struct
	 * of the program (crefl_wrap(t, &cfg)). nothing is copied, w is
	 * only a typed view of pkt.
	 */
	unsigned char pkt[64] = { 0 };
	crefl_val_t w = crefl_wrap(t, pkt);
	uint64_t hi;

	crefl_copy(w, v); /* serialize v into pkt */
	crefl_get_u64(crefl_get(w, "range[1].hi"), &hi);
	printf("pkt: hi=%llu\n", (unsigned long long)hi);

	/* introspection: all named types, then the members of cfg_t */
	for (size_t i = 0; i < crefl_ntypes(ctx); i++)
		printf("type %s\n", crefl_name(crefl_type_at(ctx, i)));

	const crefl_type_t *rt = crefl_resolve(t); /* typedef to struct */
	for (size_t i = 0; i < crefl_len(rt); i++) {
		const crefl_member_t *m = crefl_member(rt, i);
		printf("%s off=%zu size=%zu\n", m->name ? m->name : "-", m->off,
		       crefl_sizeof(m->type));
	}

	const crefl_type_t *e = crefl_type(ctx, "enum mode");
	for (size_t i = 0; i < crefl_len(e); i++)
		printf("%s=%lld\n", crefl_enumval(e, i)->name,
		       (long long)crefl_enumval(e, i)->val);

	crefl_print(t, stdout); /* C declaration with layout comments */

	/* values from crefl_new() are freed separately from the context */
	crefl_free(v);
	crefl_ctx_free(ctx);
	return 0;
}
