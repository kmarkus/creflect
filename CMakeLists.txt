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
