# SPDX-License-Identifier: LGPL-2.0-or-later
# Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>

cmake_minimum_required(VERSION 3.16)

# single source of the version: crefl.h
file(STRINGS src/crefl.h CREFL_VERSION_DEFS REGEX "^#define CREFL_VERSION_")
foreach(part MAJOR MINOR PATCH)
	string(REGEX REPLACE ".*CREFL_VERSION_${part} ([0-9]+).*" "\\1" CREFL_${part} "${CREFL_VERSION_DEFS}")
endforeach()
project(creflect VERSION ${CREFL_MAJOR}.${CREFL_MINOR}.${CREFL_PATCH} LANGUAGES C)

include(CTest)
include(GNUInstallDirs)

# optimized unless asked otherwise, cmake defaults to no -O at all
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "build type" FORCE)
endif()

# "make test": parallel, show output of failing tests (cmake >= 3.17)
cmake_host_system_information(RESULT CREFL_NCPU QUERY NUMBER_OF_LOGICAL_CORES)
set(CMAKE_CTEST_ARGUMENTS -j${CREFL_NCPU} --output-on-failure)

option(BUILD_SHARED_LIBS "build libcreflect as a shared library, OFF: static" ON)
option(CREFL_SANITIZE "build unit tests with ASan/UBSan" ON)
option(CREFL_FUZZ "build the libFuzzer harness (clang only)" OFF)
set(CREFL_RANDOM_SEEDS 10 CACHE STRING "random type headers per oracle configuration")
set(CREFL_RANDOM_FIRST 1 CACHE STRING "first random seed, CI rotates it")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CREFL_SRC src/crefl.c src/crefl_parse.c)
set(CREFL_WARN -Wall -Wextra)

add_library(creflect ${CREFL_SRC})
target_include_directories(creflect PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(creflect PRIVATE ${CREFL_WARN})
set_target_properties(creflect PROPERTIES PUBLIC_HEADER src/crefl.h
	VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
if(BUILD_SHARED_LIBS)
	# export the API only, not the crefl__ internals
	target_link_options(creflect PRIVATE
		-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/crefl.map)
	set_target_properties(creflect PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/crefl.map)
endif()

configure_file(src/creflect.pc.in creflect.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/creflect.pc
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

add_executable(crefl-tool tools/crefl-tool.c)
target_link_libraries(crefl-tool PRIVATE creflect)
target_compile_options(crefl-tool PRIVATE ${CREFL_WARN})

install(TARGETS creflect crefl-tool
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Lua bindings, one module per Lua in CONFIG_LUA_VER: a list (commas
# or semicolons) of 5.1..5.5, jit or lua (the unversioned lua.pc, as
# installed by OE). Empty: the newest Lua found. CONFIG_LUA_BIN +
# CONFIG_LUA_INCDIR: one module for that interpreter (luarocks).
option(CREFL_LUA "build the Lua bindings" ON)
set(CONFIG_LUA_VER "" CACHE STRING "Lua versions to build for, e.g. jit,5.4")
set(CONFIG_LUA_BIN "" CACHE FILEPATH "Lua interpreter to build against")
set(CONFIG_LUA_INCDIR "" CACHE PATH "Lua include dir to build against")
# relative to the install prefix, lib/ as in Lua's default package.cpath
set(CONFIG_LIBDIR "lib/lua" CACHE STRING
	"Lua C module dir, modules go to <dir>/<version>")

# module for one Lua: entry from CONFIG_LUA_VER, or "" for CONFIG_LUA_BIN
function(crefl_lua entry)
	string(MAKE_C_IDENTIFIER "CREFL_LUA_${entry}" P)
	if("${entry}" STREQUAL "")
		set(${P}_FOUND ON)
		set(${P}_INCLUDE_DIRS "${CONFIG_LUA_INCDIR}")
		set(bin "${CONFIG_LUA_BIN}")
		# ask the interpreter itself, so luajit reports its 5.1 ABI version
		execute_process(COMMAND ${bin} -e "io.write(_VERSION:match('%d+%.%d+'))"
			OUTPUT_VARIABLE ver COMMAND_ERROR_IS_FATAL ANY)
		execute_process(COMMAND ${bin} -e "io.write(jit and 'jit' or '')"
			OUTPUT_VARIABLE jit)
	else()
		if("${entry}" STREQUAL "newest")
			set(mods lua5.5 lua5.4 lua5.3 luajit lua5.2 lua5.1 lua)
		elseif("${entry}" STREQUAL "jit")
			set(mods luajit REQUIRED)
		elseif("${entry}" STREQUAL "lua")
			set(mods lua REQUIRED)
		else()
			set(mods lua${entry} REQUIRED)
		endif()
		pkg_search_module(${P} ${mods})
		if(NOT ${P}_FOUND)
			message(STATUS "lua: skipped, no Lua found")
			return()
		endif()
		if("${${P}_MODULE_NAME}" STREQUAL "luajit")
			pkg_get_variable(ver luajit abiver)
			set(jit jit)
		else()
			# remove minor version (meta-oe pc file has no major_version)
			string(REGEX MATCH "^([0-9]+\\.[0-9]+)" ver ${${P}_VERSION})
			set(jit "")
		endif()
	endif()

	# luajit and lua 5.1 would install to the same lib/lua/5.1
	get_property(dirs GLOBAL PROPERTY CREFL_LUA_DIRS)
	if("${ver}" IN_LIST dirs)
		message(FATAL_ERROR "lua: two modules for lib/lua/${ver} (${entry})")
	endif()
	set_property(GLOBAL APPEND PROPERTY CREFL_LUA_DIRS ${ver})

	if(jit)
		set(name luajit)
	else()
		set(name lua${ver})
	endif()
	message(STATUS "lua: ${name} (${ver})")

	if("${ver}" STREQUAL "5.1" OR "${ver}" STREQUAL "5.2")
		# debian ships it for lua5.1 only, it supports 5.2 too
		find_file(COMPAT53 compat-5.3.h PATH_SUFFIXES "lua${ver}" lua5.1)
		if(NOT COMPAT53)
			message(FATAL_ERROR "failed to find compat-5.3.h for lua${ver}, please install lua-compat53-dev")
		endif()
		get_filename_component(compat_dir ${COMPAT53} DIRECTORY)
	endif()

	# library compiled in, only luaopen_creflect is exported
	set(t creflect-${name})
	add_library(${t} MODULE lua/creflect_lua.c ${CREFL_SRC})
	target_include_directories(${t} PRIVATE src ${${P}_INCLUDE_DIRS} ${compat_dir})
	target_compile_options(${t} PRIVATE ${CREFL_WARN} -fvisibility=hidden)
	set_target_properties(${t} PROPERTIES PREFIX "" OUTPUT_NAME creflect
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${name})
	# self-contained module, so LTO can inline the library (10-20% faster)
	if(CREFL_IPO)
		set_property(TARGET ${t} PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
	endif()
	install(TARGETS ${t} LIBRARY DESTINATION ${CONFIG_LIBDIR}/${ver})

	if(NOT BUILD_TESTING)
		return()
	endif()
	if("${bin}" STREQUAL "")
		# cached per module, the interpreter of one must not leak into the next
		if(jit)
			find_program(${P}_BIN luajit)
		else()
			find_program(${P}_BIN NAMES lua${ver})
		endif()
		set(bin ${${P}_BIN})
	endif()
	if(NOT bin)
		message(STATUS "lua tests: skipped for ${name}, interpreter not found")
		return()
	endif()
	# tests and luaunit load from the default package.path
	set(env LUA_CPATH=${CMAKE_CURRENT_BINARY_DIR}/${name}/?.so)
	execute_process(COMMAND ${bin} -e "require 'luaunit'"
		RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
	if(rc EQUAL 0)
		add_test(NAME lua-${name}
			COMMAND ${CMAKE_COMMAND} -E env ${env} ${bin} run.lua
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lua/test)
	else()
		message(STATUS "lua tests: skipped for ${name}, luaunit not found")
	endif()
	file(GLOB examples ${CMAKE_CURRENT_SOURCE_DIR}/lua/examples/*.lua)
	foreach(ex ${examples})
		get_filename_component(n ${ex} NAME_WE)
		add_test(NAME lua-${name}-example-${n}
			COMMAND ${CMAKE_COMMAND} -E env ${env} ${bin} ${ex}
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/lua/examples)
	endforeach()
endfunction()

if(CREFL_LUA)
	find_package(PkgConfig REQUIRED)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT CREFL_IPO LANGUAGES C)
	if(NOT "${CONFIG_LUA_INCDIR}" STREQUAL "")
		crefl_lua("")
	elseif("${CONFIG_LUA_VER}" STREQUAL "")
		crefl_lua(newest)
	else()
		string(REPLACE "," ";" vers "${CONFIG_LUA_VER}")
		foreach(v ${vers})
			crefl_lua(${v})
		endforeach()
	endif()
endif()

if(BUILD_TESTING)
	find_package(cmocka REQUIRED)

	# sources compiled in so the sanitizers cover the library too
	add_executable(crefl-test test/crefl-test.c ${CREFL_SRC})
	target_include_directories(crefl-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_compile_definitions(crefl-test PRIVATE _POSIX_C_SOURCE=200809L)
	target_compile_options(crefl-test PRIVATE ${CREFL_WARN})
	target_link_libraries(crefl-test PRIVATE cmocka::cmocka)
	if(CREFL_SANITIZE)
		target_compile_options(crefl-test PRIVATE
			-fsanitize=address,undefined -fno-omit-frame-pointer)
		target_link_options(crefl-test PRIVATE -fsanitize=address,undefined)
	endif()
	add_test(NAME unit COMMAND crefl-test)

	# kernel coding style, see .clang-format
	find_program(CLANG_FORMAT clang-format)
	if(CLANG_FORMAT)
		file(GLOB CREFL_STYLE_SRC src/*.[ch] tools/*.c test/*.[ch] lua/*.c examples/*.c)
		add_test(NAME style COMMAND ${CLANG_FORMAT} --dry-run --Werror ${CREFL_STYLE_SRC})
	else()
		message(STATUS "style: skipped, clang-format not found")
	endif()

	include(test/crefl-tool.cmake)

	# README examples: built and run, and kept identical to the README
	add_executable(example-readme examples/readme.c)
	target_link_libraries(example-readme PRIVATE creflect)
	target_compile_options(example-readme PRIVATE ${CREFL_WARN})
	add_test(NAME example-readme COMMAND example-readme)
	add_test(NAME readme COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/readme-check.sh)

	# compiler oracle: one test per compiler/ABI that can build and run
	file(GLOB CREFL_TESTCASES ${CMAKE_CURRENT_SOURCE_DIR}/test/testcases/*.h)

	function(crefl_oracle name cc flags run)
		find_program(_cc_${name} ${cc})
		if(run)
			find_program(_run_${name} ${run})
		else()
			set(_run_${name} "")
		endif()
		if(NOT _cc_${name} OR (run AND NOT _run_${name}))
			message(STATUS "oracle-${name}: skipped, ${cc} or ${run} not found")
			return()
		endif()

		# probe: can it link and run a trivial program?
		set(probe ${CMAKE_CURRENT_BINARY_DIR}/probe-${name})
		file(WRITE ${probe}.c "int main(void) { return 0; }\n")
		separate_arguments(flag_list UNIX_COMMAND "${flags}")
		execute_process(COMMAND ${_cc_${name}} ${flag_list} ${probe}.c -o ${probe}
			RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
		if(rc EQUAL 0)
			execute_process(COMMAND ${_run_${name}} ${probe}
				RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
		endif()
		if(NOT rc EQUAL 0)
			message(STATUS "oracle-${name}: skipped, ${cc} ${flags} cannot build/run")
			return()
		endif()

		add_test(NAME oracle-${name}
			COMMAND ${CMAKE_COMMAND} -E env
				CC=${_cc_${name}} "ABI_CFLAGS=${flags}" RUN=${_run_${name}}
				${CMAKE_CURRENT_SOURCE_DIR}/test/oracle.sh
				${CMAKE_CURRENT_BINARY_DIR}/oracle/${name} ${CREFL_TESTCASES})
		message(STATUS "oracle-${name}: enabled")

		if(Python3_FOUND)
			add_test(NAME oracle-random-${name}
				COMMAND ${CMAKE_COMMAND} -E env
					CC=${_cc_${name}} "ABI_CFLAGS=${flags}" RUN=${_run_${name}} JOBS=4
					${CMAKE_CURRENT_SOURCE_DIR}/test/random-oracle.sh
					${CMAKE_CURRENT_BINARY_DIR}/oracle-random/${name}
					${CREFL_RANDOM_FIRST} ${CREFL_RANDOM_SEEDS})
			# many seeds under qemu take longer than the default 1500 s
			set_tests_properties(oracle-random-${name} PROPERTIES
				PROCESSORS 4 TIMEOUT 3600)
		endif()
	endfunction()

	find_package(Python3 COMPONENTS Interpreter)
	if(NOT Python3_FOUND)
		message(STATUS "oracle-random-*: skipped, python3 not found")
	endif()

	crefl_oracle(gcc gcc "" "")
	crefl_oracle(gcc-m32 gcc "-m32" "")
	crefl_oracle(clang clang "" "")
	crefl_oracle(clang-m32 clang "-m32" "")
	crefl_oracle(s390x s390x-linux-gnu-gcc "-static" qemu-s390x)
	crefl_oracle(armhf arm-linux-gnueabihf-gcc "-static" qemu-arm)
	crefl_oracle(arm64 aarch64-linux-gnu-gcc "-static" qemu-aarch64)
endif()

if(CREFL_FUZZ)
	if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "CREFL_FUZZ needs clang: -DCMAKE_C_COMPILER=clang")
	endif()
	add_executable(crefl-fuzz test/fuzz.c ${CREFL_SRC})
	target_include_directories(crefl-fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
	target_compile_options(crefl-fuzz PRIVATE -O1 -fsanitize=fuzzer,address,undefined)
	target_link_options(crefl-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
