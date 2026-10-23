function(pulseforge_apply_visual_studio_debugger_working_directory Target)
	if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
		return()
	endif()

	# Keep generator expressions out of the Open Folder File API. Resolve the active
	# Visual Studio output directory late, after the selected configuration sets OutDir.
	set(PULSEFORGE_DEBUGGER_PROPERTIES_FILE "${CMAKE_CURRENT_BINARY_DIR}/PulseForgeDebugger.props")
	set(PULSEFORGE_DEBUGGER_TARGETS_FILE "${CMAKE_CURRENT_BINARY_DIR}/PulseForgeDebugger.targets")
	file(WRITE "${PULSEFORGE_DEBUGGER_PROPERTIES_FILE}"
		"<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
		"  <PropertyGroup>\n"
		"    <ForceImportBeforeCppTargets>$(MSBuildThisFileDirectory)PulseForgeDebugger.targets</ForceImportBeforeCppTargets>\n"
		"  </PropertyGroup>\n"
		"</Project>\n")
	file(WRITE "${PULSEFORGE_DEBUGGER_TARGETS_FILE}"
		"<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
		"  <PropertyGroup>\n"
		"    <LocalDebuggerWorkingDirectory>$(OutDir)</LocalDebuggerWorkingDirectory>\n"
		"  </PropertyGroup>\n"
		"</Project>\n")
	set_property(TARGET "${Target}" PROPERTY VS_USER_PROPS "${PULSEFORGE_DEBUGGER_PROPERTIES_FILE}")
endfunction()

function(pulseforge_register_visual_studio_launch_paths_test SourceRoot)
	if(NOT BUILD_TESTING OR NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
		return()
	endif()

	set(PULSEFORGE_LAUNCH_TARGETS)
	foreach(PULSEFORGE_TARGET IN ITEMS PulseForgeEditor PulseForgeGame)
		if(TARGET "${PULSEFORGE_TARGET}")
			list(APPEND PULSEFORGE_LAUNCH_TARGETS "${PULSEFORGE_TARGET}")
		endif()
	endforeach()
	if(NOT PULSEFORGE_LAUNCH_TARGETS)
		return()
	endif()

	add_test(NAME PulseForge.CMake.VisualStudioLaunchPaths
		COMMAND "${CMAKE_COMMAND}"
			"-DPULSEFORGE_BINARY_DIR=${CMAKE_BINARY_DIR}"
			"-DPULSEFORGE_MSBUILD_EXECUTABLE=${CMAKE_MAKE_PROGRAM}"
			"-DPULSEFORGE_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
			"-DPULSEFORGE_TARGETS=${PULSEFORGE_LAUNCH_TARGETS}"
			-P "${SourceRoot}/Tests/CMake/TestVisualStudioLaunchPaths.cmake")
endfunction()
