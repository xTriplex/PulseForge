cmake_minimum_required(VERSION 3.24)

foreach(RequiredVariable IN ITEMS PULSEFORGE_REPOSITORY_ROOT PULSEFORGE_CI_DEPENDENCY_CACHE)
	if(NOT DEFINED ${RequiredVariable} OR "${${RequiredVariable}}" STREQUAL "")
		message(FATAL_ERROR "${RequiredVariable} must be provided")
	endif()
endforeach()

cmake_path(ABSOLUTE_PATH PULSEFORGE_REPOSITORY_ROOT NORMALIZE OUTPUT_VARIABLE RepositoryRoot)
cmake_path(ABSOLUTE_PATH PULSEFORGE_CI_DEPENDENCY_CACHE NORMALIZE OUTPUT_VARIABLE DependencyCache)
cmake_path(IS_PREFIX RepositoryRoot "${DependencyCache}" NORMALIZE IsInsideRepository)
if(NOT IsInsideRepository OR IS_SYMLINK "${DependencyCache}")
	message(FATAL_ERROR "The CI dependency cache must be a real directory inside the repository")
endif()

set(DependencyCacheRelative "${DependencyCache}")
cmake_path(RELATIVE_PATH DependencyCacheRelative BASE_DIRECTORY "${RepositoryRoot}")
if(NOT DependencyCacheRelative STREQUAL "Build/CI/Dependencies")
	message(FATAL_ERROR "Refusing to inspect or clean an unexpected dependency cache path: ${DependencyCache}")
endif()

function(ValidateCachedSource RelativePath)
	set(SourceDirectory "${DependencyCache}/${RelativePath}")
	if(NOT EXISTS "${SourceDirectory}")
		return()
	endif()
	if(IS_SYMLINK "${SourceDirectory}" OR NOT IS_DIRECTORY "${SourceDirectory}")
		message(FATAL_ERROR "Cached dependency source is not a real directory: ${SourceDirectory}")
	endif()

	set(IsComplete TRUE)
	foreach(Marker IN LISTS ARGN)
		if(NOT EXISTS "${SourceDirectory}/${Marker}" OR IS_SYMLINK "${SourceDirectory}/${Marker}")
			set(IsComplete FALSE)
		endif()
	endforeach()
	if(NOT IsComplete)
		message(STATUS "Removing incomplete cached dependency source ${RelativePath}; FetchContent will repopulate it online")
		file(REMOVE_RECURSE "${SourceDirectory}")
	endif()
endfunction()

# The cache key includes the pinned declarations. These markers detect truncated
# archives; fully disconnected FetchContent remains disabled in both CI jobs.
ValidateCachedSource("pulseforgenvrhi-src" CMakeLists.txt include/nvrhi/nvrhi.h src/vulkan/vulkan-backend.h)
ValidateCachedSource("pulseforgelua-src" src/lua.h src/lapi.c src/ldo.c)
ValidateCachedSource("vulkan_headers-src" CMakeLists.txt include/vulkan/vulkan.h)

message(STATUS "Dependency source cache validated; FetchContent may repopulate missing sources online")
