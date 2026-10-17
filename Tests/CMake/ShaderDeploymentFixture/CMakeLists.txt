cmake_minimum_required(VERSION 3.28)
project(PulseForgeShaderDeploymentFixture LANGUAGES CXX)

if(NOT PULSEFORGE_DEPLOYMENT_MODULE OR NOT EXISTS "${PULSEFORGE_DEPLOYMENT_MODULE}")
	message(FATAL_ERROR "PULSEFORGE_DEPLOYMENT_MODULE must point to PulseForgeShaderDeployment.cmake")
endif()
include("${PULSEFORGE_DEPLOYMENT_MODULE}")

set(ShaderSource "${CMAKE_CURRENT_BINARY_DIR}/fixture-shader.source")
set(ShaderOutput "${CMAKE_CURRENT_BINARY_DIR}/generated/fixture-shader.spv")
set(ShaderBuildCount "${CMAKE_CURRENT_BINARY_DIR}/shader-build-count.txt")
file(WRITE "${ShaderSource}" "shader version one")
add_custom_command(
	OUTPUT "${ShaderOutput}"
	COMMAND "${CMAKE_COMMAND}" "-DPULSEFORGE_SHADER_SOURCE=${ShaderSource}"
		"-DPULSEFORGE_SHADER_OUTPUT=${ShaderOutput}" "-DPULSEFORGE_SHADER_BUILD_COUNT=${ShaderBuildCount}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateShader.cmake"
	DEPENDS "${ShaderSource}"
	VERBATIM
)
add_custom_target(FixtureShaderAssets DEPENDS "${ShaderOutput}")
add_executable(FixtureRuntime fixture.cpp)
pulseforge_attach_shader_deployment(FixtureRuntime "${CMAKE_CURRENT_BINARY_DIR}/runtime/Shaders"
	ASSET_TARGETS FixtureShaderAssets SHADERS "${ShaderOutput}")
