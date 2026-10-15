function(pf_collect_quality_targets Directory Output)
	get_property(DirectoryTargets DIRECTORY "${Directory}" PROPERTY BUILDSYSTEM_TARGETS)
	set(CollectedTargets)
	foreach(Target IN LISTS DirectoryTargets)
		if(NOT Target MATCHES "^PulseForge")
			continue()
		endif()
		get_target_property(IsImported ${Target} IMPORTED)
		if(NOT IsImported)
			list(APPEND CollectedTargets "${Target}")
		endif()
	endforeach()

	get_property(Subdirectories DIRECTORY "${Directory}" PROPERTY SUBDIRECTORIES)
	foreach(Subdirectory IN LISTS Subdirectories)
		pf_collect_quality_targets("${Subdirectory}" SubdirectoryTargets)
		list(APPEND CollectedTargets ${SubdirectoryTargets})
	endforeach()
	set(${Output} "${CollectedTargets}" PARENT_SCOPE)
endfunction()

pf_collect_quality_targets("${CMAKE_SOURCE_DIR}" PulseForgeQualityTargets)
list(REMOVE_DUPLICATES PulseForgeQualityTargets)
list(SORT PulseForgeQualityTargets)

set(PulseForgeQualityGraph "CONFIG|$<CONFIG>\n")
string(APPEND PulseForgeQualityGraph "OPTION|PULSEFORGE_BUILD_EDITOR|${PULSEFORGE_BUILD_EDITOR}\n")
string(APPEND PulseForgeQualityGraph "OPTION|BUILD_TESTING|${BUILD_TESTING}\n")
foreach(Target IN LISTS PulseForgeQualityTargets)
	get_target_property(TargetType ${Target} TYPE)
	string(APPEND PulseForgeQualityGraph "TARGET|${Target}|${TargetType}\n")
	string(APPEND PulseForgeQualityGraph
		"SOURCES|${Target}|$<TARGET_GENEX_EVAL:${Target},$<TARGET_PROPERTY:${Target},SOURCES>>\n")
	string(APPEND PulseForgeQualityGraph
		"LINKS|${Target}|$<TARGET_GENEX_EVAL:${Target},$<TARGET_PROPERTY:${Target},LINK_LIBRARIES>>\n")
	string(APPEND PulseForgeQualityGraph
		"INTERFACE|${Target}|$<TARGET_GENEX_EVAL:${Target},$<TARGET_PROPERTY:${Target},INTERFACE_LINK_LIBRARIES>>\n")
	string(APPEND PulseForgeQualityGraph
		"DEFINITIONS|${Target}|$<TARGET_GENEX_EVAL:${Target},$<TARGET_PROPERTY:${Target},COMPILE_DEFINITIONS>>\n")
endforeach()

file(GENERATE
	OUTPUT "${CMAKE_BINARY_DIR}/PulseForgeTargetGraph-$<CONFIG>.txt"
	CONTENT "${PulseForgeQualityGraph}"
)
