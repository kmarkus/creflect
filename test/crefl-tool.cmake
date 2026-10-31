# SPDX-License-Identifier: LGPL-2.0-or-later
# Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>

# crefl-tool command line tests, included from CMakeLists.txt

set(TC ${CMAKE_CURRENT_SOURCE_DIR}/test/testcases)
set(TD ${CMAKE_CURRENT_SOURCE_DIR}/test/crefl-tool)

# crefl_tool_test(name regex args...): output must match regex
function(crefl_tool_test name regex)
	add_test(NAME crefl-tool-${name} COMMAND crefl-tool ${ARGN})
	set_tests_properties(crefl-tool-${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

# crefl_tool_fail(name args...): must exit non-zero
function(crefl_tool_fail name)
	add_test(NAME crefl-tool-${name} COMMAND crefl-tool ${ARGN})
	set_tests_properties(crefl-tool-${name} PROPERTIES WILL_FAIL TRUE)
endfunction()

crefl_tool_test(help "usage: crefl-tool" -h)
crefl_tool_test(list "struct bf_basic\nstruct bf_full\n" -l ${TC}/30-bitfield-basic.h)
crefl_tool_test(print "struct bf_basic {.*unsigned int a : 3;" ${TC}/30-bitfield-basic.h)
crefl_tool_test(type "^/\\* size=[0-9]+, align=[0-9]+ \\*/\nstruct bf_full {"
	-t "struct bf_full" ${TC}/30-bitfield-basic.h)
crefl_tool_test(path-bitfield "e: off=[0-9]+ size=8 bit=[0-7] width=40 type=uint64_t"
	-t "struct bf_mixed" -p e ${TC}/31-bitfield-mixed-types.h)
crefl_tool_test(path-multi "b: off=2 size=2 bit=0 width=9 type=uint16_t\nj: off=[0-9]+ size=4 type=int"
	-t "struct bf_mixed" -p b -p j ${TC}/31-bitfield-mixed-types.h)
crefl_tool_test(path-array "start\\[9\\].day: off=224 size=8 type=double"
	-E -t "struct bonk" -p "start[9].day" ${TC}/04-struct-with-array-of-structs.h)
crefl_tool_test(define "a: off=0 size=7 type=<array>\nb: off=8 size=4 type=int"
	-D N=7 -t "struct d" -p a -p b ${TD}/define.h)
crefl_tool_test(cpp-include "struct date start;"
	-E -I ${TC} -t "struct period" ${TC}/03-nested-struct.h)
crefl_tool_test(cpp-define "a: off=0 size=3 type="
	-E -D N=3 -t "struct d" -p a ${TD}/define.h)
crefl_tool_test(cpp-multi "struct c\nstruct a\nstruct b\n"
	-E -l ${TD}/a.h ${TD}/b.h)
crefl_tool_test(extern-c "struct in_c" -l ${TD}/externc.h)
crefl_tool_test(quiet "^$" -q ${TC}/30-bitfield-basic.h)
crefl_tool_test(parse-error "broken.h: line 3: unknown type name 'foo_t'" ${TD}/broken.h)
crefl_tool_test(no-type "struct nosuch: incomplete type"
	-t "struct nosuch" ${TC}/30-bitfield-basic.h)
crefl_tool_test(bad-path "nosuch: No such file or directory"
	-t "struct bf_basic" -p nosuch ${TC}/30-bitfield-basic.h)
crefl_tool_test(decode "^tag = \"ABCD\"\na = 49\nb = 45\nc = 13107\nmode = 892679477 \\(M\\)\n$"
	-t "struct rec" -d ${TD}/decode1.bin ${TD}/decode.h)
crefl_tool_test(decode-multi "\\[0\\]\\.c = 13107\n\n\\[1\\]\\.c = 13107\n.*1 trailing bytes ignored"
	-t "struct rec" -d ${TD}/decode2.bin -p c ${TD}/decode.h)
crefl_tool_test(decode-array "^\\[0\\] = 65\n\\[1\\] = 66\n"
	-t "uint8_t[12]" -d ${TD}/decode1.bin ${TD}/decode.h)

crefl_tool_fail(fail-parse ${TD}/broken.h)
crefl_tool_fail(fail-option -x)
crefl_tool_fail(fail-path-without-type -p a ${TC}/30-bitfield-basic.h)
crefl_tool_fail(fail-list-and-type -l -t int ${TC}/30-bitfield-basic.h)
crefl_tool_fail(fail-missing-file ${TD}/nosuch.h)
crefl_tool_fail(fail-define-value -D N=x ${TD}/define.h)
crefl_tool_fail(fail-undefined ${TD}/define.h)
crefl_tool_fail(fail-cpp -E ${TD}/nosuch.h)
crefl_tool_fail(fail-cpp-multi-stdin -E ${TD}/a.h -)
crefl_tool_fail(fail-decode-without-type -d ${TD}/decode1.bin ${TD}/decode.h)
crefl_tool_fail(fail-decode-path -t "struct rec" -d ${TD}/decode1.bin -p x ${TD}/decode.h)
crefl_tool_fail(fail-decode-missing -t "struct rec" -d ${TD}/nosuch.bin ${TD}/decode.h)
crefl_tool_fail(fail-define-name -D 1x ${TD}/define.h)
