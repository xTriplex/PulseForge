if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
	if(NOT BUILD_TESTING)
		message(FATAL_ERROR "PULSEFORGE_ENABLE_QUALITY_CHECKS requires BUILD_TESTING=ON")
	endif()
	if(NOT MSVC)
		message(FATAL_ERROR "PulseForge quality validation requires the supported MSVC /analyze workflow")
	endif()
	if(NOT PULSEFORGE_ENABLE_MSVC_ANALYSIS)
		message(FATAL_ERROR
			"PULSEFORGE_ENABLE_QUALITY_CHECKS requires PULSEFORGE_ENABLE_MSVC_ANALYSIS=ON; "
			"ordinary builds leave compiler analysis disabled by default.")
	endif()

	set(PULSEFORGE_QUALITY_TOOLS_DIR "" CACHE PATH
		"External directory containing the PulseForge quality scripts and rules")
	if(PULSEFORGE_QUALITY_TOOLS_DIR STREQUAL "")
		message(FATAL_ERROR
			"Explicit quality validation requires PULSEFORGE_QUALITY_TOOLS_DIR to point to the external Quality directory "
			"containing the PulseForge quality scripts and rules.")
	endif()
	if(NOT IS_DIRECTORY "${PULSEFORGE_QUALITY_TOOLS_DIR}")
		message(FATAL_ERROR
			"PULSEFORGE_QUALITY_TOOLS_DIR='${PULSEFORGE_QUALITY_TOOLS_DIR}' is not an existing directory. "
			"Set it to the external Quality directory, for example <toolkit-root>/Tools/Quality.")
	endif()
	file(REAL_PATH "${PULSEFORGE_QUALITY_TOOLS_DIR}" QualityToolRoot)
	set(PULSEFORGE_QUALITY_TOOLS_DIR "${QualityToolRoot}" CACHE PATH
		"External directory containing the PulseForge quality scripts and rules" FORCE)

	set(PULSEFORGE_QUALITY_PYTHON_EXECUTABLE "" CACHE FILEPATH
		"Optional explicit Python override with pinned PulseForge quality tools installed")
	set(QualityRequiredFiles
		"${QualityToolRoot}/BootstrapQualityTools.py"
		"${QualityToolRoot}/CheckSourceQuality.py"
		"${QualityToolRoot}/CheckCodeQualityTools.py"
		"${QualityToolRoot}/ExportConfiguredTargetGraph.cmake"
		"${QualityToolRoot}/SourceQualityRules.json"
		"${QualityToolRoot}/requirements.txt"
	)
	foreach(QualityFile IN LISTS QualityRequiredFiles)
		if(NOT EXISTS "${QualityFile}")
			message(FATAL_ERROR
				"Explicit quality validation is unavailable because required file '${QualityFile}' is missing. "
				"Check PULSEFORGE_QUALITY_TOOLS_DIR='${QualityToolRoot}' and restore the missing external toolkit file.")
		endif()
	endforeach()

	set(QualityPythonDefault "${CMAKE_CURRENT_SOURCE_DIR}/Build/QualityTools/.venv/Scripts/python.exe")
	if(PULSEFORGE_QUALITY_PYTHON_EXECUTABLE STREQUAL "")
		set(QualityPythonSelected "${QualityPythonDefault}")
	else()
		set(QualityPythonSelected "${PULSEFORGE_QUALITY_PYTHON_EXECUTABLE}")
	endif()
	if(NOT EXISTS "${QualityPythonSelected}")
		message(FATAL_ERROR
			"Explicit quality validation requires Python at '${QualityPythonSelected}'. "
			"Bootstrap the shared pinned environment with: "
			"python \"${QualityToolRoot}/BootstrapQualityTools.py\" --venv \"${CMAKE_CURRENT_SOURCE_DIR}/Build/QualityTools/.venv\" "
			"or set PULSEFORGE_QUALITY_PYTHON_EXECUTABLE to a compatible interpreter. "
			"An explicitly supplied invalid override is not replaced automatically.")
	endif()
	execute_process(
		COMMAND "${QualityPythonSelected}" -c
			"import importlib.metadata as m, sys; print(f'{sys.version_info.major}.{sys.version_info.minor} lizard {m.version(\"lizard\")}, pathspec {m.version(\"pathspec\")}, Pygments {m.version(\"Pygments\")}')"
		OUTPUT_VARIABLE QualityPythonVersion
		ERROR_VARIABLE QualityPythonError
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE QualityPythonResult
	)
	if(NOT "${QualityPythonResult}" STREQUAL "0" OR
		NOT QualityPythonVersion MATCHES "^3\\.(9|[1-9][0-9]) lizard 1\\.24\\.1, pathspec 1\\.1\\.1, Pygments 2\\.21\\.0$")
		string(STRIP "${QualityPythonError}" QualityPythonError)
		message(FATAL_ERROR
			"Quality Python at '${QualityPythonSelected}' is missing required packages or versions. "
			"Expected Python >=3.9 with Lizard 1.24.1, pathspec 1.1.1, and Pygments 2.21.0; "
			"received '${QualityPythonVersion}' (exit ${QualityPythonResult}): ${QualityPythonError}. "
			"Bootstrap Build/QualityTools/.venv with \"${QualityToolRoot}/BootstrapQualityTools.py\" "
			"or correct the explicit interpreter override.")
	endif()
	message(STATUS "PulseForge quality tools: Python ${QualityPythonVersion} (${QualityPythonSelected})")

	set(ClangFormatHints
		"$ENV{VSINSTALLDIR}/VC/Tools/Llvm/bin"
		"$ENV{VSINSTALLDIR}/VC/Tools/Llvm/x64/bin"
		"$ENV{VCToolsInstallDir}/../../Llvm/bin"
		"$ENV{VCToolsInstallDir}/../../Llvm/x64/bin"
		"${CMAKE_VS_INSTALL_PATH}/VC/Tools/Llvm/bin"
		"${CMAKE_VS_INSTALL_PATH}/VC/Tools/Llvm/x64/bin"
	)
	file(GLOB VisualStudioLlvmHints
		"$ENV{ProgramFiles}/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/bin"
		"$ENV{ProgramFiles}/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/x64/bin"
	)
	list(APPEND ClangFormatHints ${VisualStudioLlvmHints})
	find_program(PULSEFORGE_CLANG_FORMAT_EXECUTABLE
		NAMES clang-format.exe clang-format
		HINTS ${ClangFormatHints}
		DOC "clang-format 19.1.5 used by PulseForge changed-code checks"
	)
	if(NOT PULSEFORGE_CLANG_FORMAT_EXECUTABLE)
		message(FATAL_ERROR
			"Required clang-format 19.1.5 was not found. Install the Visual Studio LLVM tools component "
			"or set PULSEFORGE_CLANG_FORMAT_EXECUTABLE to its executable path.")
	endif()
	execute_process(
		COMMAND "${PULSEFORGE_CLANG_FORMAT_EXECUTABLE}" --version
		OUTPUT_VARIABLE ClangFormatVersion
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE ClangFormatResult
	)
	if(NOT ClangFormatResult EQUAL 0 OR NOT ClangFormatVersion MATCHES "clang-format version 19\\.1\\.5")
		message(FATAL_ERROR
			"PulseForge formatting requires clang-format 19.1.5; found '${ClangFormatVersion}' at "
			"${PULSEFORGE_CLANG_FORMAT_EXECUTABLE}")
	endif()
	message(STATUS "PulseForge changed-code formatter: ${ClangFormatVersion} (${PULSEFORGE_CLANG_FORMAT_EXECUTABLE})")
	message(STATUS "MSVC Code Analysis is enabled for importer, mesh-data, editor-core, editor-UI and scene-render owners")

	file(READ "${QualityToolRoot}/SourceQualityRules.json" PULSEFORGE_QUALITY_RULES_JSON)

	function(pf_add_public_header_check_target Target PublicRoot)
		file(GLOB_RECURSE PublicHeaders CONFIGURE_DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/${PublicRoot}/*.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/${PublicRoot}/*.hpp"
		)
		string(JSON NonApiHeaderCount LENGTH "${PULSEFORGE_QUALITY_RULES_JSON}" non_api_public_headers)
		set(NonApiHeaders)
		if(NonApiHeaderCount GREATER 0)
			math(EXPR LastNonApiHeader "${NonApiHeaderCount} - 1")
			foreach(Index RANGE 0 ${LastNonApiHeader})
				string(JSON NonApiHeader GET "${PULSEFORGE_QUALITY_RULES_JSON}" non_api_public_headers ${Index})
				list(APPEND NonApiHeaders "${NonApiHeader}")
			endforeach()
		endif()
		string(JSON ImplementationHeaderCount LENGTH "${PULSEFORGE_QUALITY_RULES_JSON}" implementation_public_headers)
		set(ImplementationHeaders)
		if(ImplementationHeaderCount GREATER 0)
			math(EXPR LastImplementationHeader "${ImplementationHeaderCount} - 1")
			foreach(Index RANGE 0 ${LastImplementationHeader})
				string(JSON ImplementationHeader GET "${PULSEFORGE_QUALITY_RULES_JSON}"
					implementation_public_headers ${Index})
				list(APPEND ImplementationHeaders "${ImplementationHeader}")
			endforeach()
		endif()
		set(OwnerTargets)
		set(ImplementationCheckOwners)
		foreach(Header IN LISTS PublicHeaders)
			file(RELATIVE_PATH HeaderPath "${CMAKE_CURRENT_SOURCE_DIR}" "${Header}")
			file(TO_CMAKE_PATH "${HeaderPath}" HeaderPath)
			file(RELATIVE_PATH IncludePath "${CMAKE_CURRENT_SOURCE_DIR}/${PublicRoot}" "${Header}")
			string(JSON HeaderOwner ERROR_VARIABLE OwnerError GET
				"${PULSEFORGE_QUALITY_RULES_JSON}" public_header_owners "${HeaderPath}")
			if(OwnerError)
				if(HeaderPath IN_LIST NonApiHeaders)
					continue()
				endif()
				message(FATAL_ERROR "Public header ${HeaderPath} has no owner-scoped compile target")
			endif()
			if(NOT TARGET "${HeaderOwner}")
				message(FATAL_ERROR "Public header ${HeaderPath} names unavailable owner target ${HeaderOwner}")
			endif()
			string(MD5 HeaderID "${HeaderPath}")
			set(CheckSource "${CMAKE_CURRENT_BINARY_DIR}/PublicHeaderChecks/${Target}/${HeaderOwner}/${HeaderID}.cpp")
			get_filename_component(CheckDirectory "${CheckSource}" DIRECTORY)
			file(MAKE_DIRECTORY "${CheckDirectory}")
			set(CheckContent "#include \"${IncludePath}\"\nstatic_assert(true);\n")
			if(EXISTS "${CheckSource}")
				file(READ "${CheckSource}" ExistingCheckContent)
				if(NOT "${ExistingCheckContent}" STREQUAL "${CheckContent}")
					file(WRITE "${CheckSource}" "${CheckContent}")
				endif()
			else()
				file(WRITE "${CheckSource}" "${CheckContent}")
			endif()
			if(HeaderPath IN_LIST ImplementationHeaders)
				# These headers follow the Source/Public layout but remain implementation
				# contracts. Compile them in the actual owner's private context without its PCH.
				set_source_files_properties("${CheckSource}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
				target_sources("${HeaderOwner}" PRIVATE "${CheckSource}")
				if(NOT HeaderOwner IN_LIST ImplementationCheckOwners)
					list(APPEND ImplementationCheckOwners "${HeaderOwner}")
				endif()
			else()
				set(OwnerSources_${HeaderOwner} ${OwnerSources_${HeaderOwner}} "${CheckSource}")
				if(NOT HeaderOwner IN_LIST OwnerTargets)
					list(APPEND OwnerTargets "${HeaderOwner}")
				endif()
			endif()
		endforeach()
		set(CheckTargets ${ImplementationCheckOwners})
		foreach(HeaderOwner IN LISTS OwnerTargets)
			set(CheckTarget "${Target}_${HeaderOwner}")
			add_library(${CheckTarget} OBJECT ${OwnerSources_${HeaderOwner}})
			target_compile_options(${CheckTarget} PRIVATE
				$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
				$<$<CXX_COMPILER_ID:MSVC>:/utf-8>
				$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
			)
			target_compile_definitions(${CheckTarget} PRIVATE
				PF_PLATFORM_WINDOWS
				$<$<CONFIG:Debug>:PF_DEBUG>
				$<$<CONFIG:Release>:PF_RELEASE>
				$<$<CONFIG:Dist>:PF_DIST>
			)
			target_link_libraries(${CheckTarget} PRIVATE "${HeaderOwner}")
			list(APPEND CheckTargets "${CheckTarget}")
		endforeach()
		add_custom_target(${Target} DEPENDS ${CheckTargets})
	endfunction()

	pf_add_public_header_check_target(PulseForgeEnginePublicHeaders PulseForge/Source/Public)
	pf_add_public_header_check_target(PulseForgeGamePublicHeaders PulseForgeGame/Source/Public)
	set(PULSEFORGE_PUBLIC_HEADER_CHECK_TARGETS
		PulseForgeEnginePublicHeaders
		PulseForgeGamePublicHeaders
	)
	if(PULSEFORGE_BUILD_EDITOR)
		pf_add_public_header_check_target(PulseForgeEditorPublicHeaders PulseForgeEditor/Source/Public)
		list(APPEND PULSEFORGE_PUBLIC_HEADER_CHECK_TARGETS PulseForgeEditorPublicHeaders)
	endif()
	add_custom_target(PulseForgePublicHeaderChecks DEPENDS ${PULSEFORGE_PUBLIC_HEADER_CHECK_TARGETS})

	set(PULSEFORGE_QUALITY_COMPARISON_BASE "" CACHE STRING
		"Git revision used to check all first-party source changed by this branch")
	if(PULSEFORGE_QUALITY_COMPARISON_BASE STREQUAL "")
		execute_process(
			COMMAND git merge-base origin/main HEAD
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			OUTPUT_VARIABLE PULSEFORGE_QUALITY_COMPARISON_BASE
			OUTPUT_STRIP_TRAILING_WHITESPACE
			RESULT_VARIABLE QualityBaseResult
		)
		if(NOT QualityBaseResult EQUAL 0 OR PULSEFORGE_QUALITY_COMPARISON_BASE STREQUAL "")
			message(FATAL_ERROR
				"Source quality checks require origin/main or an explicit PULSEFORGE_QUALITY_COMPARISON_BASE revision")
		endif()
	endif()
	message(STATUS "PulseForge source quality comparison base: ${PULSEFORGE_QUALITY_COMPARISON_BASE}")
	add_test(NAME PulseForge.SourceQuality
		COMMAND "${QualityPythonSelected}" -B "${QualityToolRoot}/CheckSourceQuality.py"
			--root "${CMAKE_CURRENT_SOURCE_DIR}"
			--rules "${QualityToolRoot}/SourceQualityRules.json"
			--graph-manifest "${CMAKE_BINARY_DIR}/PulseForgeTargetGraph-$<CONFIG>.txt"
			--comparison-base "${PULSEFORGE_QUALITY_COMPARISON_BASE}")
	add_test(NAME PulseForge.SourceQualityRules
		COMMAND "${QualityPythonSelected}" -B -m unittest discover
			-s "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Quality" -p test_*.py)
	add_test(NAME PulseForge.CodeQualityTools
		COMMAND "${QualityPythonSelected}" -B "${QualityToolRoot}/CheckCodeQualityTools.py"
			--root "${CMAKE_CURRENT_SOURCE_DIR}"
			--comparison-base "${PULSEFORGE_QUALITY_COMPARISON_BASE}"
			--configuration "$<CONFIG>"
			--clang-format "${PULSEFORGE_CLANG_FORMAT_EXECUTABLE}"
			--metrics-output "${CMAKE_BINARY_DIR}/QualityReports/FunctionMetrics-$<CONFIG>.json")
	set_tests_properties(PulseForge.SourceQuality PulseForge.SourceQualityRules PulseForge.CodeQualityTools PROPERTIES
		ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PULSEFORGE_QUALITY_TOOLS_DIR=${QualityToolRoot};PULSEFORGE_QUALITY_CMAKE_COMMAND=${CMAKE_COMMAND};PULSEFORGE_QUALITY_GENERATOR=${CMAKE_GENERATOR};PULSEFORGE_QUALITY_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM};PULSEFORGE_QUALITY_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM};PULSEFORGE_CLANG_FORMAT_EXECUTABLE=${PULSEFORGE_CLANG_FORMAT_EXECUTABLE}")
endif()
