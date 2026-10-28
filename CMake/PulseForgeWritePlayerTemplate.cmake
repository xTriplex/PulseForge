if(NOT DEFINED PF_TEMPLATE_ROOT OR NOT DEFINED PF_PLAYER_FILE OR NOT DEFINED PF_RESOURCE_DIR OR
	NOT DEFINED PF_SOURCE_ROOT OR NOT DEFINED PF_LUA_SOURCE_DIR OR NOT DEFINED PF_NVRHI_SOURCE_DIR OR
	NOT DEFINED PF_CONFIGURATION OR NOT DEFINED PF_PLATFORM OR NOT DEFINED PF_ARCHITECTURE OR
	NOT DEFINED PF_DESCRIPTOR)
	message(FATAL_ERROR "PulseForge player template descriptor arguments are incomplete")
endif()
if(NOT EXISTS "${PF_PLAYER_FILE}" OR NOT EXISTS "${PF_RESOURCE_DIR}/Shaders")
	message(FATAL_ERROR "Cooked player template requires its executable and deployed Shaders directory")
endif()
set(THIRD_PARTY_NOTICES "PulseForge runtime third-party notices\n")
set(THIRD_PARTY_LICENSES
	"GLFW|${PF_SOURCE_ROOT}/PulseForge/Vendor/Glfw/LICENSE.md"
	"GLM|${PF_SOURCE_ROOT}/PulseForge/Vendor/glm/copying.txt"
	"EnTT|${PF_SOURCE_ROOT}/PulseForge/Vendor/entt/LICENSE"
	"Jolt Physics|${PF_SOURCE_ROOT}/PulseForge/Vendor/JoltPhysics/LICENSE"
	"nlohmann/json|${PF_SOURCE_ROOT}/PulseForge/Vendor/json/LICENSE.MIT"
	"miniaudio|${PF_SOURCE_ROOT}/PulseForge/Vendor/miniaudio/LICENSE"
	"spdlog|${PF_SOURCE_ROOT}/PulseForge/Vendor/spdlog/LICENSE"
	"stb|${PF_SOURCE_ROOT}/PulseForge/Vendor/stb/LICENSE"
	"Lua 5.5.1|${PF_LUA_SOURCE_DIR}/../doc/readme.html"
	"NVRHI|${PF_NVRHI_SOURCE_DIR}/LICENSE.txt")
if(PF_PLATFORM STREQUAL "macOS")
	list(APPEND THIRD_PARTY_LICENSES
		"Khronos Vulkan Loader 1.4.350.0 (Apache 2.0 applies to most files. Some source files use more permissive licenses. Upstream LICENSE.txt: https://github.com/KhronosGroup/Vulkan-Loader/blob/vulkan-sdk-1.4.350.0/LICENSE.txt)|${PF_SOURCE_ROOT}/PulseForge/Vendor/json/LICENSES/Apache-2.0.txt"
		"MoltenVK 1.4.1 (Apache License 2.0. Upstream LICENSE: https://github.com/KhronosGroup/MoltenVK/blob/v1.4.1/LICENSE)|${PF_SOURCE_ROOT}/PulseForge/Vendor/json/LICENSES/Apache-2.0.txt")
endif()
foreach(LICENSE_ENTRY IN LISTS THIRD_PARTY_LICENSES)
	string(FIND "${LICENSE_ENTRY}" "|" SEPARATOR)
	string(SUBSTRING "${LICENSE_ENTRY}" 0 ${SEPARATOR} COMPONENT)
	math(EXPR LICENSE_PATH_BEGIN "${SEPARATOR} + 1")
	string(SUBSTRING "${LICENSE_ENTRY}" ${LICENSE_PATH_BEGIN} -1 LICENSE_PATH)
	if(NOT EXISTS "${LICENSE_PATH}")
		message(FATAL_ERROR "Required runtime license text for ${COMPONENT} was not found: ${LICENSE_PATH}")
	endif()
	file(READ "${LICENSE_PATH}" LICENSE_TEXT)
	string(APPEND THIRD_PARTY_NOTICES "\n========================================\n${COMPONENT}\n========================================\n\n${LICENSE_TEXT}\n")
endforeach()
file(MAKE_DIRECTORY "${PF_RESOURCE_DIR}")
file(WRITE "${PF_RESOURCE_DIR}/ThirdPartyNotices.txt" "${THIRD_PARTY_NOTICES}")
if(PF_PLATFORM STREQUAL "macOS")
	set(PF_APP_BUNDLE "${PF_TEMPLATE_ROOT}")
	set(PF_EXECUTABLE "${PF_PLAYER_FILE}")
	include("${CMAKE_CURRENT_LIST_DIR}/PulseForgeStageMacOSVulkanRuntime.cmake")
endif()
file(RELATIVE_PATH PLAYER_RELATIVE "${PF_TEMPLATE_ROOT}" "${PF_PLAYER_FILE}")
file(TO_CMAKE_PATH "${PLAYER_RELATIVE}" PLAYER_RELATIVE)
if(PLAYER_RELATIVE MATCHES "^\.\./" OR IS_ABSOLUTE "${PLAYER_RELATIVE}")
	message(FATAL_ERROR "Cooked player executable is outside its template root")
endif()
set(RUNTIME_FILES "${PLAYER_RELATIVE}")
file(RELATIVE_PATH NOTICES_RELATIVE "${PF_TEMPLATE_ROOT}" "${PF_RESOURCE_DIR}/ThirdPartyNotices.txt")
file(TO_CMAKE_PATH "${NOTICES_RELATIVE}" NOTICES_RELATIVE)
if(NOTICES_RELATIVE MATCHES "^\.\./" OR IS_ABSOLUTE "${NOTICES_RELATIVE}")
	message(FATAL_ERROR "Runtime notices are outside the player template root")
endif()
list(APPEND RUNTIME_FILES "${NOTICES_RELATIVE}")
file(GLOB_RECURSE SHADER_FILES LIST_DIRECTORIES FALSE "${PF_RESOURCE_DIR}/Shaders/*")
foreach(FILE_PATH IN LISTS SHADER_FILES)
	if(NOT IS_SYMLINK "${FILE_PATH}")
		file(RELATIVE_PATH RELATIVE_FILE "${PF_TEMPLATE_ROOT}" "${FILE_PATH}")
		file(TO_CMAKE_PATH "${RELATIVE_FILE}" RELATIVE_FILE)
		if(RELATIVE_FILE MATCHES "^\.\./" OR IS_ABSOLUTE "${RELATIVE_FILE}")
			message(FATAL_ERROR "Cooked shader is outside its template root")
		endif()
		if(RELATIVE_FILE MATCHES "(^|/)EditorImGui(Present)?\\.")
			continue()
		endif()
		list(APPEND RUNTIME_FILES "${RELATIVE_FILE}")
	endif()
endforeach()
if(PF_PLATFORM STREQUAL "macOS")
	foreach(BUNDLE_METADATA IN ITEMS "Contents/Info.plist" "Contents/PkgInfo")
		if(EXISTS "${PF_TEMPLATE_ROOT}/${BUNDLE_METADATA}")
			list(APPEND RUNTIME_FILES "${BUNDLE_METADATA}")
		endif()
	endforeach()
	file(GLOB_RECURSE PRIVATE_FRAMEWORK_FILES LIST_DIRECTORIES FALSE "${PF_TEMPLATE_ROOT}/Contents/Frameworks/*")
	foreach(FILE_PATH IN LISTS PRIVATE_FRAMEWORK_FILES)
		file(RELATIVE_PATH RELATIVE_FILE "${PF_TEMPLATE_ROOT}" "${FILE_PATH}")
		file(TO_CMAKE_PATH "${RELATIVE_FILE}" RELATIVE_FILE)
		list(APPEND RUNTIME_FILES "${RELATIVE_FILE}")
	endforeach()
	file(GLOB_RECURSE ICD_FILES LIST_DIRECTORIES FALSE "${PF_RESOURCE_DIR}/vulkan/icd.d/*")
	foreach(FILE_PATH IN LISTS ICD_FILES)
		file(RELATIVE_PATH RELATIVE_FILE "${PF_TEMPLATE_ROOT}" "${FILE_PATH}")
		file(TO_CMAKE_PATH "${RELATIVE_FILE}" RELATIVE_FILE)
		list(APPEND RUNTIME_FILES "${RELATIVE_FILE}")
	endforeach()
	file(GLOB_RECURSE RESOURCE_FILES LIST_DIRECTORIES FALSE "${PF_RESOURCE_DIR}/*")
	foreach(FILE_PATH IN LISTS RESOURCE_FILES)
		if(NOT IS_SYMLINK "${FILE_PATH}")
			file(RELATIVE_PATH RELATIVE_FILE "${PF_TEMPLATE_ROOT}" "${FILE_PATH}")
			file(TO_CMAKE_PATH "${RELATIVE_FILE}" RELATIVE_FILE)
			list(APPEND RUNTIME_FILES "${RELATIVE_FILE}")
		endif()
	endforeach()
endif()
list(REMOVE_DUPLICATES RUNTIME_FILES)
list(SORT RUNTIME_FILES)
set(JSON_FILES "[")
set(FIRST TRUE)
foreach(FILE IN LISTS RUNTIME_FILES)
	string(REPLACE "\\" "\\\\" ESCAPED "${FILE}")
	string(REPLACE "\"" "\\\"" ESCAPED "${ESCAPED}")
	if(NOT FIRST)
		string(APPEND JSON_FILES ",")
	endif()
	string(APPEND JSON_FILES "\"${ESCAPED}\"")
	set(FIRST FALSE)
endforeach()
string(APPEND JSON_FILES "]")
file(WRITE "${PF_DESCRIPTOR}"
	"{\n"
	"  \"format\": \"PulseForgePlayerTemplate\",\n"
	"  \"version\": 1,\n"
	"  \"targetPlatform\": \"${PF_PLATFORM}\",\n"
	"  \"architecture\": \"${PF_ARCHITECTURE}\",\n"
	"  \"configuration\": \"${PF_CONFIGURATION}\",\n"
	"  \"renderer\": \"Vulkan\",\n"
	"  \"runtimeABI\": \"pulseforge-runtime-v1\",\n"
	"  \"cookManifestSchema\": 2,\n"
	"  \"entrypoint\": \"${PLAYER_RELATIVE}\",\n"
	"  \"runtimeFiles\": ${JSON_FILES}\n"
	"}\n")
