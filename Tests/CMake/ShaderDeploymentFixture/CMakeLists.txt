cmake_minimum_required(VERSION 3.28)
project(PulseForgeShaderDeploymentFixture LANGUAGES CXX)

if(NOT PULSEFORGE_DEPLOYMENT_MODULE OR NOT EXISTS "${PULSEFORGE_DEPLOYMENT_MODULE}")
	message(FATAL_ERROR "PULSEFORGE_DEPLOYMENT_MODULE must point to PulseForgeShaderDeployment.cmake")
endif()
include("${PULSEFORGE_DEPLOYMENT_MODULE}")

set(ShaderSource "${CMAKE_CURRENT_BINARY_DIR}/fixture-shader.source")
set(ShaderOutput "${CMAKE_CURRENT_BINARY_DIR}/generated/fixture-shader.spv")
file(WRITE "${ShaderSource}" "shader version one")
add_custom_command(
	OUTPUT "${ShaderOutput}"
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
	COMMAND "${CMAKE_COMMAND}" -E copy "${ShaderSource}" "${ShaderOutput}"
	DEPENDS "${ShaderSource}"
	VERBATIM
)
add_executable(FixtureRuntime fixture.cpp)
pulseforge_attach_shader_deployment(FixtureRuntime "${CMAKE_CURRENT_BINARY_DIR}/runtime/Shaders" "${ShaderOutput}")
