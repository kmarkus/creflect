// SPDX-License-Identifier: LGPL-2.0-or-later
/*
 * libFuzzer harness: parse arbitrary input, print what was parsed
 *
 * Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crefl.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static FILE *null;
	crefl_ctx_t *ctx = crefl_ctx_new();
	char *src = malloc(size + 1);

	if (null == NULL)
		null = fopen("/dev/null", "w");

	memcpy(src, data, size);
	src[size] = '\0';

	crefl_parse(ctx, src);
	for (size_t i = 0; i < crefl_ntypes(ctx); i++)
		crefl_print(crefl_type_at(ctx, i), null);
	crefl_type(ctx, src);

	free(src);
	crefl_ctx_free(ctx);
	return 0;
}
