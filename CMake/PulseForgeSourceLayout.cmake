cmake_policy(PUSH)
cmake_policy(VERSION 3.24)

if(NOT DEFINED PULSEFORGE_SOURCE_LAYOUT_ROOT)
	get_filename_component(PULSEFORGE_SOURCE_LAYOUT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(_pulseforge_header_extensions
	.h .hpp .hh .hxx .inl .ipp .inc
)
set(_pulseforge_implementation_extensions
	.c .cc .cpp .cxx
)
set(_pulseforge_layout_violations)

foreach(_project IN ITEMS PulseForge PulseForgeEditor PulseForgeGame)
	foreach(_scope IN ITEMS Public Private)
		set(_directory "${PULSEFORGE_SOURCE_LAYOUT_ROOT}/${_project}/Source/${_scope}")
		if(NOT IS_DIRECTORY "${_directory}")
			continue()
		endif()

		if(CMAKE_SCRIPT_MODE_FILE)
			file(GLOB_RECURSE _files LIST_DIRECTORIES FALSE "${_directory}/*")
		else()
			# This is validation only. Compilation source lists remain explicitly owned by their targets.
			file(GLOB_RECURSE _files CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE "${_directory}/*")
		endif()

		foreach(_file IN LISTS _files)
			get_filename_component(_extension "${_file}" LAST_EXT)
			string(TOLOWER "${_extension}" _extension)
			set(_violation FALSE)
			if(_scope STREQUAL "Private" AND _extension IN_LIST _pulseforge_header_extensions)
				set(_violation TRUE)
			elseif(_scope STREQUAL "Public" AND _extension IN_LIST _pulseforge_implementation_extensions)
				set(_violation TRUE)
			endif()

			if(_violation)
				file(RELATIVE_PATH _relative "${PULSEFORGE_SOURCE_LAYOUT_ROOT}" "${_file}")
				file(TO_CMAKE_PATH "${_relative}" _relative)
				list(APPEND _pulseforge_layout_violations "${_relative}")
			endif()
		endforeach()
	endforeach()
endforeach()

if(_pulseforge_layout_violations)
	list(JOIN _pulseforge_layout_violations "\n  " _formatted_violations)
	message(FATAL_ERROR
		"PulseForge first-party source layout violations were found. Headers belong under Source/Public and C/C++ "
		"implementations belong under Source/Private:\n  ${_formatted_violations}")
endif()

cmake_policy(POP)
