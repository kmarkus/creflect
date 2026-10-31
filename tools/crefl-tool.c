// SPDX-License-Identifier: LGPL-2.0-or-later
/*
 * crefl-tool: parse C type declarations and print or query them
 *
 * Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
 */

#define _DEFAULT_SOURCE /* mkstemps, realpath */

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "crefl.h"

#define MAX_ARGS 256

static const char usage[] =
	"usage: crefl-tool [options] [file...]\n"
	"\n"
	"Parse C type declarations from files (default: stdin) and print them.\n"
	"\n"
	"  -E          run input through the preprocessor ($CPP, default \"cc -E -P\")\n"
	"  -I dir      include directory for -E\n"
	"  -D n[=v]    define integer constant n (default 1), also passed to -E\n"
	"  -l          list type names, one per line\n"
	"  -t type     print only this type, e.g. \"struct foo\", foo_t, \"int[4]\"\n"
	"  -p path     with -t: print offset, size and bits of a member, e.g. a.b[2]\n"
	"  -d file     with -t: decode file as records of the type and print the\n"
	"              values (with -p: only those members)\n"
	"  -q          quiet, only check that the input parses\n"
	"  -h          show this help\n"
	"\n"
	"exit status: 0 ok, 1 parse or lookup error, 2 usage error\n";

static char *slurp(FILE *f)
{
	size_t len = 0, cap = 4096, n;
	char *buf = malloc(cap), *tmp;

	while (buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (len == cap - 1) {
			tmp = realloc(buf, cap * 2);
			if (tmp == NULL) {
				free(buf);
				return NULL;
			}
			buf = tmp;
			cap *= 2;
		}
	}
	if (buf == NULL || ferror(f)) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

/* run the preprocessor on @file ("-" for stdin), return its output */
static char *preprocess(char **cpp_args, int ncpp, const char *file);

/*
 * preprocess several files in one run, so include guards work across
 * them: a temporary file includes them all
 */
static char *preprocess_all(char **cpp_args, int ncpp, char **files, int n)
{
	const char *dir = getenv("TMPDIR");
	char tmp[PATH_MAX], path[PATH_MAX], *out = NULL;
	FILE *f;
	int fd;

	snprintf(tmp, sizeof(tmp), "%s/crefl-tool-XXXXXX.h",
		 dir && *dir ? dir : "/tmp");
	fd = mkstemps(tmp, 2);
	if (fd < 0) {
		perror("crefl-tool: temporary file");
		return NULL;
	}
	f = fdopen(fd, "w");
	if (f == NULL) {
		perror("crefl-tool: temporary file");
		close(fd);
		unlink(tmp);
		return NULL;
	}

	for (int i = 0; i < n; i++) {
		if (strcmp(files[i], "-") == 0 ||
		    realpath(files[i], path) == NULL || strchr(path, '"')) {
			fprintf(stderr, "crefl-tool: -E: %s: cannot include\n",
				files[i]);
			goto out;
		}
		fprintf(f, "#include \"%s\"\n", path);
	}
	if (fflush(f) == 0)
		out = preprocess(cpp_args, ncpp, tmp);
out:
	fclose(f);
	unlink(tmp);
	return out;
}

static char *preprocess(char **cpp_args, int ncpp, const char *file)
{
	char *argv[MAX_ARGS];
	int argc = 0, fd[2], status;
	char *cpp = getenv("CPP"), *tok, *out;
	FILE *f;
	pid_t pid;

	cpp = strdup(cpp && *cpp ? cpp : "cc -E -P");
	if (cpp == NULL)
		return NULL;

	/* $CPP is split at spaces, no quoting */
	for (tok = strtok(cpp, " "); tok && argc < MAX_ARGS - ncpp - 2;
	     tok = strtok(NULL, " "))
		argv[argc++] = tok;
	for (int i = 0; i < ncpp; i++)
		argv[argc++] = cpp_args[i];
	argv[argc++] = (char *)file;
	argv[argc] = NULL;

	if (pipe(fd)) {
		perror("crefl-tool: preprocessor");
		free(cpp);
		return NULL;
	}
	pid = fork();
	if (pid < 0) {
		perror("crefl-tool: preprocessor");
		close(fd[0]);
		close(fd[1]);
		free(cpp);
		return NULL;
	}

	if (pid == 0) {
		close(fd[0]);
		dup2(fd[1], STDOUT_FILENO);
		execvp(argv[0], argv);
		perror(argv[0]);
		_exit(127);
	}

	close(fd[1]);
	f = fdopen(fd[0], "r");
	if (f) {
		out = slurp(f);
		fclose(f);
	} else {
		out = NULL;
		close(fd[0]); /* cpp gets SIGPIPE instead of blocking */
	}

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			status = -1;
			break;
		}
	}
	free(cpp);

	if (out == NULL || status == -1 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		fprintf(stderr, "crefl-tool: preprocessor failed on %s\n",
			file);
		free(out);
		return NULL;
	}
	return out;
}

static void print_name(const crefl_type_t *t, FILE *f)
{
	switch (crefl_kind(t)) {
	case CREFL_STRUCT:
		fprintf(f, "struct ");
		break;
	case CREFL_UNION:
		fprintf(f, "union ");
		break;
	case CREFL_ENUM:
		fprintf(f, "enum ");
		break;
	default:
		break;
	}
	fprintf(f, "%s", crefl_name(t) ? crefl_name(t) : "<anonymous>");
}

static void print_kind(const crefl_type_t *t)
{
	static const char *const kinds[] = {
		[CREFL_PRIM] = "prim",	     [CREFL_PTR] = "pointer",
		[CREFL_ARRAY] = "array",     [CREFL_STRUCT] = "struct",
		[CREFL_UNION] = "union",     [CREFL_ENUM] = "enum",
		[CREFL_TYPEDEF] = "typedef",
	};

	if (crefl_name(t))
		print_name(t, stdout);
	else
		printf("<%s>", kinds[crefl_kind(t)]);
}

static void print_str(const char *s, size_t n)
{
	putchar('"');
	for (size_t i = 0; i < n && s[i]; i++) {
		unsigned char c = s[i];

		if (c == '"' || c == '\\')
			printf("\\%c", c);
		else if (isprint(c))
			putchar(c);
		else
			printf("\\x%02x", c);
	}
	putchar('"');
}

static void print_scalar(crefl_val_t v, const crefl_type_t *t)
{
	int64_t i;
	uint64_t u = 0; /* the float path leaves it unset */
	double d;

	if (crefl_kind(t) == CREFL_PTR) {
		if (crefl_get_u64(v, &u))
			goto err;
		printf("0x%llx", (unsigned long long)u);
	} else if (crefl_kind(t) == CREFL_PRIM &&
		   crefl_prim(t) >= CREFL_FLOAT) {
		if (crefl_get_f64(v, &d))
			goto err;
		printf("%.*g", crefl_prim(t) == CREFL_FLOAT ? FLT_DIG : DBL_DIG,
		       d);
	} else if (crefl_kind(t) == CREFL_PRIM && crefl_prim(t) == CREFL_BOOL) {
		if (crefl_get_u64(v, &u))
			goto err;
		printf("%s", u ? "true" : "false");
	} else if (crefl_is_signed(t)) {
		if (crefl_get_i64(v, &i))
			goto err;
		u = (uint64_t)i;
		printf("%lld", (long long)i);
	} else {
		if (crefl_get_u64(v, &u))
			goto err;
		printf("%llu", (unsigned long long)u);
	}

	/* enums: also the name if the value has one */
	for (size_t n = 0; crefl_kind(t) == CREFL_ENUM && n < crefl_len(t);
	     n++) {
		if ((uint64_t)crefl_enumval(t, n)->val == u) {
			printf(" (%s)", crefl_enumval(t, n)->name);
			break;
		}
	}
	return;
err:
	printf("?");
}

static void dump(crefl_val_t v, const char *path);

/* anonymous members are flattened, as in C member access */
static void dump_members(crefl_val_t v, const crefl_type_t *t, const char *path)
{
	char sub[512];

	for (size_t i = 0; i < crefl_len(t); i++) {
		const crefl_member_t *m = crefl_member(t, i);
		const crefl_type_t *mt = crefl_resolve(m->type);
		crefl_val_t mv = {
			.t = m->type,
			.p = (char *)v.p + m->off,
			.bit_off = m->bit_off,
			.bit_width = m->bit_width,
		};

		if (m->name) {
			snprintf(sub, sizeof(sub), "%s%s%s", path,
				 *path ? "." : "", m->name);
			dump(mv, sub);
		} else if (crefl_kind(mt) == CREFL_STRUCT ||
			   crefl_kind(mt) == CREFL_UNION) {
			dump_members(mv, mt, path);
		}
	}
}

static void dump(crefl_val_t v, const char *path)
{
	const crefl_type_t *t = crefl_resolve(v.t);
	char sub[512];

	switch (crefl_kind(t)) {
	case CREFL_STRUCT:
	case CREFL_UNION:
		dump_members(v, t, path);
		return;
	case CREFL_ARRAY: {
		const crefl_type_t *e = crefl_resolve(crefl_elem(t));

		if (crefl_kind(e) == CREFL_PRIM &&
		    crefl_prim(e) == CREFL_CHAR) {
			printf("%s = ", path);
			print_str(v.p, crefl_len(t));
			printf("\n");
			return;
		}
		/* flexible arrays: size unknown, skipped */
		for (size_t i = 0; i < crefl_len(t); i++) {
			snprintf(sub, sizeof(sub), "%s[%zu]", path, i);
			dump(crefl_index(v, i), sub);
		}
		return;
	}
	default:
		printf("%s = ", path);
		print_scalar(v, t);
		printf("\n");
	}
}

static int decode(const crefl_type_t *t, const char *file, char **paths,
		  int npaths)
{
	FILE *f = fopen(file, "rb");
	size_t size = crefl_sizeof(t), n;
	char *buf, prefix[32] = "";
	struct stat st;
	int ret = 0, multi;

	if (f == NULL) {
		fprintf(stderr, "crefl-tool: %s: %s\n", file, strerror(errno));
		return 1;
	}
	if (!crefl_is_complete(t) || size == 0) {
		fprintf(stderr, "crefl-tool: cannot decode incomplete type\n");
		fclose(f);
		return 1;
	}
	buf = malloc(size);
	if (buf == NULL) {
		perror("crefl-tool");
		fclose(f);
		return 1;
	}
	/* several records (or a pipe): prefix each with its index */
	multi = fstat(fileno(f), &st) || !S_ISREG(st.st_mode) ||
		(size_t)st.st_size > size;

	for (size_t rec = 0; (n = fread(buf, 1, size, f)) > 0; rec++) {
		crefl_val_t v = crefl_wrap(t, buf);

		if (n < size) {
			fflush(stdout);
			fprintf(stderr,
				"crefl-tool: %s: %zu trailing bytes ignored\n",
				file, n);
			break;
		}
		if (multi)
			snprintf(prefix, sizeof(prefix), "[%zu]", rec);
		if (rec)
			printf("\n");
		if (npaths == 0)
			dump(v, prefix);
		for (int i = 0; i < npaths; i++) {
			char path[512];
			crefl_val_t pv = crefl_get(v, paths[i]);

			if (pv.t == NULL) {
				fprintf(stderr,
					"crefl-tool: %s: invalid path\n",
					paths[i]);
				ret = 1;
				goto out;
			}
			snprintf(path, sizeof(path), "%s%s%s", prefix,
				 *prefix && paths[i][0] != '[' ? "." : "",
				 paths[i]);
			dump(pv, path);
		}
	}
	if (ferror(f)) {
		fprintf(stderr, "crefl-tool: %s: read error\n", file);
		ret = 1;
	}
out:
	free(buf);
	fclose(f);
	return ret;
}

static int query_path(const crefl_type_t *t, const char *path)
{
	crefl_path_t p;
	int ret = crefl_path(t, path, &p);

	if (ret) {
		fprintf(stderr, "crefl-tool: %s: %s\n", path, strerror(-ret));
		return 1;
	}

	printf("%s: off=%zu size=%zu", path, p.off, crefl_sizeof(p.t));
	if (p.bit_width)
		printf(" bit=%u width=%u", p.bit_off, p.bit_width);
	printf(" type=");
	print_kind(p.t);
	printf("\n");
	return 0;
}

int main(int argc, char **argv)
{
	char *cpp_args[MAX_ARGS], *type = NULL, *paths[MAX_ARGS], *data = NULL;
	int ncpp = 0, npaths = 0, do_cpp = 0, list = 0, quiet = 0, ret = 0, opt;
	crefl_ctx_t *ctx = crefl_ctx_new();

	if (ctx == NULL) {
		perror("crefl-tool");
		return 1;
	}

	while ((opt = getopt(argc, argv, "EI:D:lt:p:d:qh")) != -1) {
		switch (opt) {
		case 'E':
			do_cpp = 1;
			break;
		case 'I':
		case 'D':
			if (ncpp >= MAX_ARGS / 2) {
				fprintf(stderr, "crefl-tool: too many -I/-D\n");
				ret = 2;
				goto out;
			}
			cpp_args[ncpp++] = opt == 'I' ? "-I" : "-D";
			cpp_args[ncpp++] = optarg;

			if (opt == 'D') {
				char *eq = strchr(optarg, '='), *end;
				long long v = 1;

				if (eq) {
					*eq = '\0';
					errno = 0;
					v = strtoll(eq + 1, &end, 0);
					if (errno || *end || end == eq + 1) {
						fprintf(stderr,
							"crefl-tool: -D %s: not an integer\n",
							optarg);
						ret = 2;
						goto out;
					}
				}
				if (crefl_define(ctx, optarg, v)) {
					fprintf(stderr, "crefl-tool: -D: %s\n",
						crefl_errmsg(ctx));
					ret = 2;
					goto out;
				}
				if (eq)
					*eq = '=';
			}
			break;
		case 'l':
			list = 1;
			break;
		case 't':
			type = optarg;
			break;
		case 'p':
			if (npaths == MAX_ARGS) {
				fprintf(stderr, "crefl-tool: too many -p\n");
				ret = 2;
				goto out;
			}
			paths[npaths++] = optarg;
			break;
		case 'd':
			data = optarg;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'h':
			fputs(usage, stdout);
			ret = 0;
			goto out;
		default:
			fputs(usage, stderr);
			ret = 2;
			goto out;
		}
	}

	if (((npaths || data) && type == NULL) || (list && type)) {
		fprintf(stderr,
			"crefl-tool: -p and -d need -t, -l and -t exclude each other\n");
		ret = 2;
		goto out;
	}

	int nfiles = argc - optind;

	/* -E with several files: one preprocessor run, one parse */
	if (do_cpp && nfiles > 1) {
		char *src =
			preprocess_all(cpp_args, ncpp, argv + optind, nfiles);

		if (src == NULL) {
			ret = 1;
			goto out;
		}
		if (crefl_parse(ctx, src)) {
			fprintf(stderr, "preprocessed input: %s\n",
				crefl_errmsg(ctx));
			ret = 1;
		}
		free(src);
		nfiles = -1;
	}

	for (int i = 0; nfiles >= 0 && i < (nfiles ? nfiles : 1); i++) {
		const char *file = nfiles ? argv[optind + i] : "-";
		char *src;

		if (do_cpp) {
			src = preprocess(cpp_args, ncpp, file);
		} else {
			FILE *f = strcmp(file, "-") ? fopen(file, "r") : stdin;

			if (f == NULL) {
				fprintf(stderr, "crefl-tool: %s: %s\n", file,
					strerror(errno));
				ret = 1;
				goto out;
			}
			src = slurp(f);
			if (f != stdin)
				fclose(f);
		}

		if (src == NULL) {
			fprintf(stderr, "crefl-tool: %s: cannot read\n", file);
			ret = 1;
			goto out;
		}
		if (crefl_parse(ctx, src)) {
			fprintf(stderr, "%s: %s\n",
				strcmp(file, "-") ? file : "<stdin>",
				crefl_errmsg(ctx));
			ret = 1;
		}
		free(src);
	}

	if (ret || quiet)
		goto out;

	if (type) {
		const crefl_type_t *t = crefl_type(ctx, type);

		if (t == NULL) {
			fprintf(stderr, "crefl-tool: %s: %s\n", type,
				*crefl_errmsg(ctx) ? crefl_errmsg(ctx) :
						     "not found");
			ret = 1;
		} else if (data) {
			ret = decode(t, data, paths, npaths);
			goto out;
		} else if (npaths == 0) {
			if (!crefl_is_complete(t)) {
				fprintf(stderr,
					"crefl-tool: %s: incomplete type\n",
					type);
				ret = 1;
			} else {
				crefl_print(t, stdout);
			}
		}
		for (int i = 0; t && i < npaths; i++)
			ret |= query_path(t, paths[i]);
		goto out;
	}

	for (size_t i = 0; i < crefl_ntypes(ctx); i++) {
		if (list) {
			print_name(crefl_type_at(ctx, i), stdout);
			printf("\n");
		} else {
			crefl_print(crefl_type_at(ctx, i), stdout);
		}
	}

out:
	crefl_ctx_free(ctx);
	return ret;
}
