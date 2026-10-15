if(NOT DEFINED PULSEFORGE_BUILD_TREE OR NOT IS_DIRECTORY "${PULSEFORGE_BUILD_TREE}")
	message(FATAL_ERROR "PULSEFORGE_BUILD_TREE must name the configured build directory")
endif()

set(CacheFile "${PULSEFORGE_BUILD_TREE}/CMakeCache.txt")
set(CTestFile "${PULSEFORGE_BUILD_TREE}/CTestTestfile.cmake")
if(NOT EXISTS "${CacheFile}" OR NOT EXISTS "${CTestFile}")
	message(FATAL_ERROR "Normal build-mode checks require CMakeCache.txt and CTestTestfile.cmake")
endif()

file(READ "${CacheFile}" CacheContents)
if(NOT CacheContents MATCHES "PULSEFORGE_ENABLE_QUALITY_CHECKS:BOOL=OFF")
	message(FATAL_ERROR "This check must run from a configuration with quality checks disabled")
endif()

file(READ "${CTestFile}" CTestContents)
foreach(QualityTest PulseForge.SourceQuality PulseForge.SourceQualityRules PulseForge.CodeQualityTools)
	string(FIND "${CTestContents}" "${QualityTest}" QualityTestPosition)
	if(NOT QualityTestPosition EQUAL -1)
		message(FATAL_ERROR "Normal C++ testing unexpectedly registered quality test ${QualityTest}")
	endif()
endforeach()

foreach(CppTest PulseForge.Engine PulseForge.AssetImports PulseForge.EditorCore)
	string(FIND "${CTestContents}" "${CppTest}" CppTestPosition)
	if(CppTestPosition EQUAL -1)
		message(FATAL_ERROR "Quality separation removed ordinary C++ test ${CppTest}")
	endif()
endforeach()

set(QualityTargetNames
	PulseForgePublicHeaderChecks
	PulseForgeEnginePublicHeaders
	PulseForgeGamePublicHeaders
	PulseForgeEditorPublicHeaders)
if(EXISTS "${PULSEFORGE_BUILD_TREE}/build.ninja")
	file(READ "${PULSEFORGE_BUILD_TREE}/build.ninja" BuildGraph)
	set(BuildTargetManifest "${BuildGraph}")
else()
	file(GLOB SolutionFiles "${PULSEFORGE_BUILD_TREE}/*.sln")
	if(NOT SolutionFiles)
		message(FATAL_ERROR "Cannot inspect configured targets for this generator")
	endif()
	list(GET SolutionFiles 0 SolutionFile)
	file(READ "${SolutionFile}" BuildTargetManifest)
endif()
foreach(QualityTarget IN LISTS QualityTargetNames)
	string(FIND "${BuildTargetManifest}" "${QualityTarget}" QualityTargetPosition)
	if(NOT QualityTargetPosition EQUAL -1)
		message(FATAL_ERROR "A quality-only target is present in an ordinary build: ${QualityTarget}")
	endif()
endforeach()

message(STATUS "Ordinary C++ tests are registered without quality-only targets or test entries")
