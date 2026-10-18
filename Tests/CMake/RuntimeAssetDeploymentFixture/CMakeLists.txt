cmake_minimum_required(VERSION 3.24)
project(PulseForgeRuntimeAssetDeploymentFixture LANGUAGES CXX)

if(NOT PULSEFORGE_DEPLOYMENT_MODULE OR NOT EXISTS "${PULSEFORGE_DEPLOYMENT_MODULE}")
	message(FATAL_ERROR "PULSEFORGE_DEPLOYMENT_MODULE must point to PulseForgeAssetDeployment.cmake")
endif()
include("${PULSEFORGE_DEPLOYMENT_MODULE}")

set(AssetSourceRoot "${CMAKE_CURRENT_BINARY_DIR}/source")
file(MAKE_DIRECTORY "${AssetSourceRoot}/Assets/Textures" "${AssetSourceRoot}/Assets/Materials")
file(WRITE "${AssetSourceRoot}/Assets/Textures/fixture.texture" "texture source version one")
file(WRITE "${AssetSourceRoot}/Assets/Materials/fixture.material" "material source version one")
file(WRITE "${AssetSourceRoot}/FixtureRuntime.pfproj" "project source version one")

add_executable(FixtureRuntime fixture.cpp)
pulseforge_attach_asset_deployment(FixtureRuntime "${CMAKE_CURRENT_BINARY_DIR}/runtime"
	DIRECTORIES
		"${AssetSourceRoot}/Assets/Textures" "Assets/Textures"
		"${AssetSourceRoot}/Assets/Materials" "Assets/Materials"
	FILES "${AssetSourceRoot}/FixtureRuntime.pfproj" "FixtureRuntime.pfproj")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fixture-target-$<CONFIG>.txt" CONTENT "$<TARGET_FILE:FixtureRuntime>")
