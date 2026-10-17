foreach(RequiredVariable IN ITEMS PULSEFORGE_FIXTURE_SOURCE_DIR PULSEFORGE_TEST_BINARY_DIR PULSEFORGE_TEST_GENERATOR
		PULSEFORGE_DEPLOYMENT_MODULE)
	if(NOT DEFINED ${RequiredVariable} OR "${${RequiredVariable}}" STREQUAL "")
		message(FATAL_ERROR "${RequiredVariable} is required for shader-deployment validation")
	endif()
endforeach()

set(GeneratorArguments -G "${PULSEFORGE_TEST_GENERATOR}")
foreach(GeneratorProperty IN ITEMS PLATFORM TOOLSET)
	set(GeneratorValue "${PULSEFORGE_TEST_GENERATOR_${GeneratorProperty}}")
	if(NOT GeneratorValue STREQUAL "")
		if(GeneratorProperty STREQUAL "PLATFORM")
			list(APPEND GeneratorArguments -A "${GeneratorValue}")
		else()
			list(APPEND GeneratorArguments -T "${GeneratorValue}")
		endif()
	endif()
endforeach()
file(REMOVE_RECURSE "${PULSEFORGE_TEST_BINARY_DIR}")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -S "${PULSEFORGE_FIXTURE_SOURCE_DIR}" -B "${PULSEFORGE_TEST_BINARY_DIR}"
		${GeneratorArguments} "-DPULSEFORGE_DEPLOYMENT_MODULE=${PULSEFORGE_DEPLOYMENT_MODULE}"
	RESULT_VARIABLE ConfigureResult OUTPUT_VARIABLE ConfigureOutput ERROR_VARIABLE ConfigureError
)
if(NOT ConfigureResult EQUAL 0)
	message(FATAL_ERROR "Could not configure shader-deployment fixture:\n${ConfigureOutput}\n${ConfigureError}")
endif()

function(build_shader_fixture)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" --build "${PULSEFORGE_TEST_BINARY_DIR}" --parallel 2
		RESULT_VARIABLE BuildResult OUTPUT_VARIABLE BuildOutput ERROR_VARIABLE BuildError
	)
	if(NOT BuildResult EQUAL 0)
		message(FATAL_ERROR "Shader-deployment fixture build failed:\n${BuildOutput}\n${BuildError}")
	endif()
endfunction()

set(DeployedShader "${PULSEFORGE_TEST_BINARY_DIR}/runtime/Shaders/fixture-shader.spv")
build_shader_fixture()
if(NOT EXISTS "${DeployedShader}")
	message(FATAL_ERROR "Initial build did not deploy its shader")
endif()
file(READ "${DeployedShader}" FirstShaderContents)
if(NOT FirstShaderContents STREQUAL "shader version one")
	message(FATAL_ERROR "Initial deployment has unexpected shader contents: '${FirstShaderContents}'")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
file(WRITE "${PULSEFORGE_TEST_BINARY_DIR}/fixture-shader.source" "shader version two")
build_shader_fixture()
file(READ "${DeployedShader}" UpdatedShaderContents)
if(NOT UpdatedShaderContents STREQUAL "shader version two")
	message(FATAL_ERROR "Shader-only update did not refresh the deployed shader")
endif()

file(REMOVE "${DeployedShader}")
build_shader_fixture()
if(NOT EXISTS "${DeployedShader}")
	message(FATAL_ERROR "An incremental build did not restore a missing deployed shader")
endif()
file(READ "${DeployedShader}" RestoredShaderContents)
if(NOT RestoredShaderContents STREQUAL "shader version two")
	message(FATAL_ERROR "Restored shader differs from its generated source")
endif()
