cmake_minimum_required(VERSION 3.24)
project(PulseForgeLaunchRegistrationFixture LANGUAGES CXX)

set(BUILD_TESTING "${PULSEFORGE_FIXTURE_TESTING}" CACHE BOOL "Enable fixture tests" FORCE)
include(CTest)

if(NOT PULSEFORGE_FIXTURE_CORE_ONLY)
	if(PULSEFORGE_FIXTURE_EDITOR)
		add_executable(PulseForgeEditor "${CMAKE_CURRENT_SOURCE_DIR}/MinimalExecutable.cpp")
	endif()
	if(PULSEFORGE_FIXTURE_GAME)
		add_executable(PulseForgeGame "${CMAKE_CURRENT_SOURCE_DIR}/MinimalExecutable.cpp")
	endif()

	include("${PULSEFORGE_SOURCE_ROOT}/CMake/PulseForgeVisualStudioLaunch.cmake")
	pulseforge_register_visual_studio_launch_paths_test("${PULSEFORGE_SOURCE_ROOT}")
endif()
