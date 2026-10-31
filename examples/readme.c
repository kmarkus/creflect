#include <errno.h>
#include <stdio.h>
#include "crefl.h"

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
	/* or crefl_ctx_new_static(buf, len) */
	crefl_ctx_t *ctx = crefl_ctx_new();

	crefl_define(ctx, "NRANGE", 2); /* macros used in declarations */
	if (crefl_parse(ctx, hdr)) {
		fprintf(stderr, "%s\n", crefl_errmsg(ctx)); /* "line 2: ..." */
		return 1;
	}

	const crefl_type_t *t = crefl_type(ctx, "cfg_t");
	crefl_val_t v = crefl_new(t); /* zeroed instance */
	double gain;
	int64_t mode;

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

	/* hot path: resolve once, then O(1) access */
	crefl_path_t p;
	uint64_t raw;
	crefl_path(t, "raw", &p);
	crefl_get_u64(crefl_at(v, &p), &raw);
	printf("raw=0x%llx\n", (unsigned long long)raw);

	/* existing data, e.g. a received packet */
	unsigned char buf[64];
	crefl_copy(crefl_wrap(t, buf), v);

	/* introspection */
	for (size_t i = 0; i < crefl_ntypes(ctx); i++)
		printf("type %s\n", crefl_name(crefl_type_at(ctx, i)));

	const crefl_type_t *rt = crefl_resolve(t);
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
	crefl_free(v);
	crefl_ctx_free(ctx);
	return 0;
}
