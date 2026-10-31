#!/bin/sh
# SPDX-License-Identifier: LGPL-2.0-or-later
# Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
#
# the README examples must match the files that are built and run

top=$(cd "$(dirname "$0")/.." && pwd)
fail=0

# block <section> <lang>: first code block of that README section
block() {
	awk -v s="## $1" -v l="\`\`\`$2" \
		'$0 == s { f = 1 } f && $0 == l { g = 1; next } g && /^```$/ { exit } g' \
		"$top/README.md"
}

check() {
	if block "$1" "$2" | diff -u - "$top/$3"; then
		echo "$3: match"
	else
		echo "$3: DIFFER from README section $1"
		fail=1
	fi
}

check Usage c examples/readme.c
check Lua lua lua/examples/readme.lua
exit $fail
