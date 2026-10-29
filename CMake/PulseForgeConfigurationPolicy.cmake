function(pulseforge_inherit_dist_release_flags FlagKind)
	set(DistFlagsVariable "CMAKE_${FlagKind}_FLAGS_DIST")
	set(ReleaseFlagsVariable "CMAKE_${FlagKind}_FLAGS_RELEASE")
	set(InheritedFlagsVariable "PULSEFORGE_DIST_INHERITED_${FlagKind}_FLAGS")
	set(FlagsDescription "${FlagKind} flags used during Dist builds")

	if("${${DistFlagsVariable}}" STREQUAL "")
		set("${DistFlagsVariable}" "${${ReleaseFlagsVariable}}" CACHE STRING "${FlagsDescription}" FORCE)
		set("${InheritedFlagsVariable}" "${${ReleaseFlagsVariable}}" CACHE INTERNAL
			"Release flags last inherited by Dist" FORCE)
		return()
	endif()

	if(NOT DEFINED ${InheritedFlagsVariable})
		if("${${DistFlagsVariable}}" STREQUAL "${${ReleaseFlagsVariable}}")
			set("${InheritedFlagsVariable}" "${${ReleaseFlagsVariable}}" CACHE INTERNAL
				"Release flags last inherited by Dist" FORCE)
		else()
			set("${InheritedFlagsVariable}" "PULSEFORGE_USER_OVERRIDE" CACHE INTERNAL
				"Release flags last inherited by Dist" FORCE)
		endif()
		return()
	endif()

	if("${${InheritedFlagsVariable}}" STREQUAL "PULSEFORGE_USER_OVERRIDE")
		return()
	endif()

	if("${${DistFlagsVariable}}" STREQUAL "${${InheritedFlagsVariable}}")
		if(NOT "${${ReleaseFlagsVariable}}" STREQUAL "${${InheritedFlagsVariable}}")
			set("${DistFlagsVariable}" "${${ReleaseFlagsVariable}}" CACHE STRING "${FlagsDescription}" FORCE)
			set("${InheritedFlagsVariable}" "${${ReleaseFlagsVariable}}" CACHE INTERNAL
				"Release flags last inherited by Dist" FORCE)
		endif()
	else()
		set("${InheritedFlagsVariable}" "PULSEFORGE_USER_OVERRIDE" CACHE INTERNAL
			"Release flags last inherited by Dist" FORCE)
	endif()
endfunction()

if(PULSEFORGE_DIST_INHERIT_RELEASE_FLAGS)
	foreach(FlagKind IN ITEMS C CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
		pulseforge_inherit_dist_release_flags(${FlagKind})
	endforeach()
endif()

function(pulseforge_apply_configuration_identity Target)
	if(NOT TARGET "${Target}")
		message(FATAL_ERROR "Cannot apply PulseForge configuration identity to missing target '${Target}'")
	endif()
	target_compile_definitions("${Target}" PRIVATE
		$<$<CONFIG:Debug>:PF_DEBUG>
		$<$<CONFIG:Release>:PF_RELEASE>
		$<$<CONFIG:Dist>:PF_DIST>
	)
endfunction()
