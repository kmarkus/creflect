#!/bin/sh
# SPDX-License-Identifier: LGPL-2.0-or-later
# Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
#
# run the compiler oracle on random types from randtypes.py
#
# usage: random-oracle.sh <outdir> <first seed> <count> [ntypes]
# env: as oracle.sh (CC, ABI_CFLAGS, RUN), JOBS (default: nproc)
#
# prints failing seeds only, exit 1 on any DIFFER

top=$(cd "$(dirname "$0")/.." && pwd)
out=$1
first=$2
count=$3
ntypes=${4:-12}
jobs=${JOBS:-$(nproc)}

mkdir -p "$out/hdr"
seed=$first
while [ "$seed" -lt $((first + count)) ]; do
	python3 "$top/test/randtypes.py" "$seed" "$ntypes" > "$out/hdr/rand-$seed.h" || exit 1
	seed=$((seed + 1))
done

# seeds are split round robin over $jobs oracle runs
j=0
while [ "$j" -lt "$jobs" ]; do
	hdrs=""
	seed=$((first + j))
	while [ "$seed" -lt $((first + count)) ]; do
		hdrs="$hdrs $out/hdr/rand-$seed.h"
		seed=$((seed + jobs))
	done
	if [ -n "$hdrs" ]; then
		"$top/test/oracle.sh" "$out/job$j" $hdrs > "$out/job$j.out" 2>&1 &
		pids="$pids $!"
	fi
	j=$((j + 1))
done

# any failing job fails the run, also without DIFFER lines (build errors)
fail=0
for pid in $pids; do
	wait "$pid" || fail=1
done

cat "$out"/job*.out | grep -v ' 0 differ$'
ntally=$(cat "$out"/job*.out | grep -c ' differ$')
if [ "$ntally" -ne "$count" ]; then
	echo "DIFFER $ntally of $count seeds reported a result"
	fail=1
fi
exit $fail
