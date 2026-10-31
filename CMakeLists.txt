# SPDX-License-Identifier: LGPL-2.0-or-later
# Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>

cmake_minimum_required(VERSION 3.16)

# single source of the version: crefl.h
file(STRINGS src/crefl.h CREFL_VERSION_DEFS REGEX "^#define CREFL_VERSION_")
foreach(part MAJOR MINOR PATCH)
	string(REGEX REPLACE ".*CREFL_VERSION_${part} ([0-9]+).*" "\\1" CREFL_${part} "${CREFL_VERSION_DEFS}")
endforeach()
project(creflect VERSION ${CREFL_MAJOR}.${CREFL_MINOR}.${CREFL_PATCH} LANGUAGES C)

include(GNUInstallDirs)

# optimized unless asked otherwise, cmake defaults to no -O at all
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "build type" FORCE)
endif()

option(BUILD_SHARED_LIBS "build libcreflect as a shared library, OFF: static" ON)

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

install(TARGETS creflect
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
