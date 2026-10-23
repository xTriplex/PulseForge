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
