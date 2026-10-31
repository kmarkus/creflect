#!/bin/sh
# SPDX-License-Identifier: LGPL-2.0-or-later
# Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
#
# check crefl layout and access against the compiler for each header
#
# usage: oracle.sh <outdir> <header>...
# env: CC (default gcc), ABI_CFLAGS (e.g. -m32), RUN (e.g. qemu-s390x)
#
# prints DIFFER lines and one tally per header, exit 1 on any DIFFER

CC=${CC:-gcc}
top=$(cd "$(dirname "$0")/.." && pwd)
out=$1
shift
mkdir -p "$out"

cc() {
	$CC $ABI_CFLAGS -w -I"$top/src" -I"$top/test" "$@"
}

# library once, checks link against it
cc -c "$top/src/crefl.c" -o "$out/crefl.o" &&
cc -c "$top/src/crefl_parse.c" -o "$out/crefl_parse.o" &&
cc -o "$out/crefl-gencheck" "$top/test/crefl-gencheck.c" "$out/crefl.o" "$out/crefl_parse.o" || exit 1

fail=0
for h in "$@"; do
	n=$(basename "$h" .h)
	hdr=$(cd "$(dirname "$h")" && pwd)/$(basename "$h")
	if ! $CC $ABI_CFLAGS -E -P "$h" > "$out/$n.i" 2> "$out/$n.build.log"; then
		echo "$n: DIFFER preprocessor failed"
		fail=1
		continue
	fi
	if ! $RUN "$out/crefl-gencheck" "$out/$n.i" "$hdr" > "$out/$n-check.c" 2>> "$out/$n.build.log"; then
		echo "$n: DIFFER generator failed: $(tail -n 1 "$out/$n.build.log")"
		fail=1
		continue
	fi
	if ! cc -o "$out/$n-check" "$out/$n-check.c" "$out/crefl.o" "$out/crefl_parse.o" \
			>> "$out/$n.build.log" 2>&1; then
		echo "$n: DIFFER check program does not build, see $out/$n.build.log"
		fail=1
		continue
	fi
	if $RUN "$out/$n-check" > "$out/$n.log" 2>&1; then
		rc=0
	else
		rc=$?
		fail=1
	fi
	grep '^DIFFER' "$out/$n.log" | head -n 20
	if [ "$rc" -gt 1 ]; then
		echo "$n: DIFFER check program exit code $rc (crash?)"
	else
		echo "$n: $(tail -n 1 "$out/$n.log")"
	fi
done
exit $fail
