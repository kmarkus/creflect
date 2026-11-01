// SPDX-License-Identifier: LGPL-2.0-or-later
/*
 * crefl-gencheck: generate a C program that checks crefl against the
 * compiler for all types in a header
 *
 * usage: crefl-gencheck <preprocessed.i> <header.h> > check.c
 *
 * The program includes the header, parses the preprocessed source
 * with crefl at runtime and compares layout and access of every
 * member with what the compiler does.
 *
 * Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crefl.h"

#define PATH_LEN 512

/* checks are split into functions, gcc is superlinear on a huge main() */
#define CHECKS_PER_FN 200

static int nfn, nchecks;

/* emit one check statement, opening a new function when needed */
#define CHECK(...) (open_fn(), printf(__VA_ARGS__))

static void open_fn(void)
{
	if (nchecks++ % CHECKS_PER_FN)
		return;
	if (nfn)
		printf("}\n\n");
	printf("static void check_%d(void)\n{\n", nfn++);
}

static char *slurp(const char *file)
{
	FILE *f = fopen(file, "r");
	char *buf = NULL;
	long len = -1;

	if (f && fseek(f, 0, SEEK_END) == 0)
		len = ftell(f);
	if (len >= 0 && fseek(f, 0, SEEK_SET) == 0)
		buf = malloc(len + 1);
	if (buf == NULL || fread(buf, 1, len, f) != (size_t)len) {
		perror(file);
		exit(1);
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}

static void emit_str(const char *s)
{
	printf("\"");
	for (; *s; s++) {
		if (*s == '\n')
			printf("\\n\"\n\"");
		else if (*s == '"' || *s == '\\')
			printf("\\%c", *s);
		else
			putchar(*s);
	}
	printf("\"");
}

static void emit_int(const char *tn, const char *path, const crefl_type_t *t,
		     unsigned int w)
{
	const crefl_type_t *rt = crefl_resolve(t);

	if (w == 0)
		w = (crefl_kind(rt) == CREFL_PRIM &&
		     crefl_prim(rt) == CREFL_BOOL) ?
			    1 :
			    crefl_sizeof(rt) * 8;

	/* 128 bit: the API reads and writes 64 bit values */
	if (w > 64)
		w = 64;

	if (crefl_is_signed(rt)) {
		if (w == 64) {
			CHECK("\tACC_I(\"%s\", %s, %s, (-9223372036854775807LL - 1));\n",
			      tn, tn, path);
			CHECK("\tACC_I(\"%s\", %s, %s, 9223372036854775807LL);\n",
			      tn, tn, path);
		} else {
			long long max = (1LL << (w - 1)) - 1;

			CHECK("\tACC_I(\"%s\", %s, %s, %lldLL);\n", tn, tn,
			      path, -max - 1);
			CHECK("\tACC_I(\"%s\", %s, %s, %lldLL);\n", tn, tn,
			      path, max);
			CHECK("\tRANGE_I(\"%s\", %s, %s, %lldLL);\n", tn, tn,
			      path, -max - 2);
			CHECK("\tRANGE_I(\"%s\", %s, %s, %lldLL);\n", tn, tn,
			      path, max + 1);
		}
	} else {
		unsigned long long max = (w == 64) ? ~0ULL : (1ULL << w) - 1;

		CHECK("\tACC_U(\"%s\", %s, %s, %lluULL);\n", tn, tn, path, max);
		CHECK("\tACC_U(\"%s\", %s, %s, %lluULL);\n", tn, tn, path,
		      0x5a5a5a5a5a5a5a5aULL & max);
		CHECK("\tRANGE_I(\"%s\", %s, %s, -1LL);\n", tn, tn, path);
		if (w < 64)
			CHECK("\tRANGE_U(\"%s\", %s, %s, %lluULL);\n", tn, tn,
			      path, max + 1);
	}
}

static void visit(const char *tn, const char *path, const crefl_type_t *t,
		  unsigned int w);

static void walk_members(const char *tn, const char *prefix,
			 const crefl_type_t *t)
{
	for (size_t i = 0; i < crefl_len(t); i++) {
		const crefl_member_t *m = crefl_member(t, i);
		char path[PATH_LEN];

		if (m->name == NULL) {
			enum crefl_kind k = crefl_kind(crefl_resolve(m->type));

			if (m->bit_width == 0 &&
			    (k == CREFL_STRUCT || k == CREFL_UNION))
				walk_members(tn, prefix,
					     crefl_resolve(m->type));
			continue;
		}

		snprintf(path, sizeof(path), "%s%s%s", prefix,
			 *prefix ? "." : "", m->name);
		visit(tn, path, m->type, m->bit_width);
	}
}

/* opaque, its bytes are not accessible from C */
static int is_va_list(const crefl_type_t *t)
{
	for (; crefl_kind(t) == CREFL_TYPEDEF; t = crefl_elem(t)) {
		if (strcmp(crefl_name(t), "__builtin_va_list") == 0)
			return 1;
	}
	return 0;
}

static void visit(const char *tn, const char *path, const crefl_type_t *t,
		  unsigned int w)
{
	const crefl_type_t *rt = crefl_resolve(t);
	char sub[PATH_LEN];

	if (is_va_list(t)) {
		CHECK("\tOFF(\"%s\", %s, %s);\n", tn, tn, path);
		return;
	}

	if (w) {
		emit_int(tn, path, t, w);
		return;
	}

	CHECK("\tOFF(\"%s\", %s, %s);\n", tn, tn, path);

	switch (crefl_kind(rt)) {
	case CREFL_PRIM:
		if (crefl_prim(rt) >= CREFL_FLOAT)
			CHECK("\tACC_F(\"%s\", %s, %s, 1.5);\n", tn, tn, path);
		else if (crefl_prim(rt) != CREFL_VOID)
			emit_int(tn, path, t, 0);
		break;
	case CREFL_ENUM:
		emit_int(tn, path, t, 0);
		break;
	case CREFL_PTR:
		CHECK("\tACC_P(\"%s\", %s, %s);\n", tn, tn, path);
		break;
	case CREFL_STRUCT:
	case CREFL_UNION:
		walk_members(tn, path, rt);
		break;
	case CREFL_ARRAY:
		if (crefl_len(rt) == 0)
			break;
		snprintf(sub, sizeof(sub), "%s[0]", path);
		visit(tn, sub, crefl_elem(rt), 0);
		if (crefl_len(rt) > 1) {
			snprintf(sub, sizeof(sub), "%s[%zu]", path,
				 crefl_len(rt) - 1);
			visit(tn, sub, crefl_elem(rt), 0);
		}
		break;
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	crefl_ctx_t *ctx = crefl_ctx_new();
	char *src, *raw;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <preprocessed.i> <header.h>\n",
			argv[0]);
		return 1;
	}

	src = slurp(argv[1]);
	raw = slurp(argv[2]);
	if (crefl_parse(ctx, src)) {
		fprintf(stderr, "%s: %s\n", argv[1], crefl_errmsg(ctx));
		return 1;
	}

	printf("#include \"check_rt.h\"\n#include \"%s\"\n\n", argv[2]);
	printf("static const char src[] =\n");
	emit_str(src);
	printf(";\n\nstatic const char raw[] =\n");
	emit_str(raw);
	printf(";\n\n");

	for (size_t i = 0; i < crefl_ntypes(ctx); i++) {
		const crefl_type_t *t = crefl_type_at(ctx, i),
				   *rt = crefl_resolve(t);
		char tn[256];

		if (!crefl_is_complete(rt))
			continue;

		/* system header internals */
		if (strncmp(crefl_name(t), "__", 2) == 0)
			continue;

		switch (crefl_kind(t)) {
		case CREFL_STRUCT:
			snprintf(tn, sizeof(tn), "struct %s", crefl_name(t));
			break;
		case CREFL_UNION:
			snprintf(tn, sizeof(tn), "union %s", crefl_name(t));
			break;
		case CREFL_ENUM:
			snprintf(tn, sizeof(tn), "enum %s", crefl_name(t));
			break;
		default:
			snprintf(tn, sizeof(tn), "%s", crefl_name(t));
			break;
		}

		CHECK("\n\tLAYOUT(\"%s\", %s);\n", tn, tn);

		if (crefl_kind(rt) == CREFL_ENUM &&
		    crefl_kind(t) == CREFL_ENUM) {
			for (size_t j = 0; j < crefl_len(rt); j++)
				CHECK("\tENUMV(\"%s\", %s);\n", tn,
				      crefl_enumval(rt, j)->name);
		}

		/* walk tags, and typedefs only if they name an anonymous composite */
		if ((crefl_kind(rt) == CREFL_STRUCT ||
		     crefl_kind(rt) == CREFL_UNION) &&
		    (t == rt || crefl_name(rt) == NULL))
			walk_members(tn, "", rt);
	}

	if (nfn)
		printf("}\n\n");

	printf("int main(void)\n{\n");
	printf("\tctx = crefl_ctx_new();\n");
	printf("\tif (crefl_parse(ctx, src)) {\n\t\tprintf(\"DIFFER parse: %%s\\n\", crefl_errmsg(ctx));\n\t\treturn 1;\n\t}\n");
	for (int i = 0; i < nfn; i++)
		printf("\tcheck_%d();\n", i);
	printf("\n\troundtrip();\n");
	printf("\traw_check(raw, %d);\n", strstr(raw, "#define") ||
						  strstr(raw, "#include \"") ||
						  strstr(raw, "#if"));
	printf("\n\treturn report();\n}\n");
	free(src);
	free(raw);
	crefl_ctx_free(ctx);
	return 0;
}
