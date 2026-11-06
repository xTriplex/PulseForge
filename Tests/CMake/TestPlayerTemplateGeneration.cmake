cmake_minimum_required(VERSION 3.24)

foreach(RequiredVariable IN ITEMS PF_TEST_SOURCE_ROOT PF_TEST_LUA_SOURCE_DIR PF_TEST_NVRHI_SOURCE_DIR PF_TEST_WRITER PF_TEST_ROOT)
	if(NOT DEFINED ${RequiredVariable})
		message(FATAL_ERROR "Player template test requires ${RequiredVariable}")
	endif()
endforeach()

file(REMOVE_RECURSE "${PF_TEST_ROOT}")
set(ValidRoot "${PF_TEST_ROOT}/valid space ü")
set(ResourceRoot "${ValidRoot}/Resources with space")
file(MAKE_DIRECTORY "${ResourceRoot}/Shaders" "${ValidRoot}/bin")
file(WRITE "${ValidRoot}/bin/PulseForge.Player.v1.exe" "fixture executable")
file(WRITE "${ResourceRoot}/Shaders/scene.v1 test.spv" "shader")

function(RunTemplate Root Player Resources Descriptor FreeTypeRoot OutResult OutOutput OutError)
	execute_process(
		COMMAND "${CMAKE_COMMAND}"
			"-DPF_TEMPLATE_ROOT=${Root}"
			"-DPF_PLAYER_FILE=${Player}"
			"-DPF_RESOURCE_DIR=${Resources}"
			"-DPF_SOURCE_ROOT=${PF_TEST_SOURCE_ROOT}"
			"-DPF_LUA_SOURCE_DIR=${PF_TEST_LUA_SOURCE_DIR}"
			"-DPF_NVRHI_SOURCE_DIR=${PF_TEST_NVRHI_SOURCE_DIR}"
			"-DPF_FREETYPE_SOURCE_DIR=${FreeTypeRoot}"
			-DPF_CONFIGURATION=Debug
			-DPF_PLATFORM=Windows
			-DPF_ARCHITECTURE=x64
			"-DPF_DESCRIPTOR=${Descriptor}"
			-P "${PF_TEST_WRITER}"
		RESULT_VARIABLE Result
		OUTPUT_VARIABLE Output
		ERROR_VARIABLE Error)
	set(${OutResult} "${Result}" PARENT_SCOPE)
	set(${OutOutput} "${Output}" PARENT_SCOPE)
	set(${OutError} "${Error}" PARENT_SCOPE)
endfunction()

set(Descriptor "${ValidRoot}/PulseForgePlayerTemplate.json")
RunTemplate("${ValidRoot}" "${ValidRoot}/bin/PulseForge.Player.v1.exe" "${ResourceRoot}" "${Descriptor}"
	"${PF_TEST_SOURCE_ROOT}/PulseForge/Vendor/freetype" FirstResult FirstOutput FirstError)
if(NOT FirstResult EQUAL 0)
	message(FATAL_ERROR "Valid Unicode/space template paths failed: ${FirstOutput}${FirstError}")
endif()
string(FIND "${FirstOutput}${FirstError}" "Invalid escape sequence" InvalidEscapeWarning)
string(FIND "${FirstOutput}${FirstError}" "CMP0010" PolicyWarning)
if(NOT InvalidEscapeWarning EQUAL -1 OR NOT PolicyWarning EQUAL -1)
	message(FATAL_ERROR "Template generation emitted malformed-regex warnings: ${FirstOutput}${FirstError}")
endif()

set(Notices "${ResourceRoot}/ThirdPartyNotices.txt")
file(READ "${Notices}" NoticeText)
foreach(ExpectedText IN ITEMS "FreeType 2, source revision 67c52a0b68eaeb7ae1f2248202924883c4a232d0"
		"The FreeType Project LICENSE" "FreeType font driver for BDF fonts" "FreeType font driver for PCF fonts"
		"GLFW" "NVRHI" "Lua 5.5.1")
	string(FIND "${NoticeText}" "${ExpectedText}" TextIndex)
	if(TextIndex EQUAL -1)
		message(FATAL_ERROR "Generated runtime notices omit required text: ${ExpectedText}")
	endif()
endforeach()
string(FIND "${NoticeText}" "GLFW" GLFWNoticeIndex)
string(FIND "${NoticeText}" "FreeType 2, source revision" FreeTypeNoticeIndex)
string(FIND "${NoticeText}" "NVRHI" NVRHINoticeIndex)
if(GLFWNoticeIndex EQUAL -1 OR FreeTypeNoticeIndex EQUAL -1 OR NVRHINoticeIndex EQUAL -1)
	message(FATAL_ERROR "Generated runtime notices did not preserve existing dependency entries")
endif()
file(READ "${Descriptor}" DescriptorText)
string(FIND "${DescriptorText}" "ThirdPartyNotices.txt" NoticeInventoryIndex)
if(NoticeInventoryIndex EQUAL -1)
	message(FATAL_ERROR "Generated template inventory omitted the runtime notices")
endif()
file(SHA256 "${Notices}" FirstNoticeHash)
file(SHA256 "${Descriptor}" FirstDescriptorHash)
RunTemplate("${ValidRoot}" "${ValidRoot}/bin/PulseForge.Player.v1.exe" "${ResourceRoot}" "${Descriptor}"
	"${PF_TEST_SOURCE_ROOT}/PulseForge/Vendor/freetype" SecondResult SecondOutput SecondError)
if(NOT SecondResult EQUAL 0)
	message(FATAL_ERROR "Repeated template generation failed: ${SecondOutput}${SecondError}")
endif()
file(SHA256 "${Notices}" SecondNoticeHash)
file(SHA256 "${Descriptor}" SecondDescriptorHash)
if(NOT FirstNoticeHash STREQUAL SecondNoticeHash OR NOT FirstDescriptorHash STREQUAL SecondDescriptorHash)
	message(FATAL_ERROR "Identical template inputs produced nondeterministic outputs")
endif()

set(MissingFreeTypeRoot "${PF_TEST_ROOT}/freetype missing notice")
file(MAKE_DIRECTORY "${MissingFreeTypeRoot}/docs" "${MissingFreeTypeRoot}/src/bdf" "${MissingFreeTypeRoot}/src/pcf")
file(COPY_FILE "${PF_TEST_SOURCE_ROOT}/PulseForge/Vendor/freetype/LICENSE.TXT" "${MissingFreeTypeRoot}/LICENSE.TXT")
file(COPY_FILE "${PF_TEST_SOURCE_ROOT}/PulseForge/Vendor/freetype/docs/FTL.TXT" "${MissingFreeTypeRoot}/docs/FTL.TXT")
file(COPY_FILE "${PF_TEST_SOURCE_ROOT}/PulseForge/Vendor/freetype/src/bdf/README" "${MissingFreeTypeRoot}/src/bdf/README")
RunTemplate("${ValidRoot}" "${ValidRoot}/bin/PulseForge.Player.v1.exe" "${ResourceRoot}" "${Descriptor}"
	"${MissingFreeTypeRoot}" MissingResult MissingOutput MissingError)
if(MissingResult EQUAL 0)
	message(FATAL_ERROR "Template generation accepted a missing required FreeType notice source")
endif()
string(FIND "${MissingOutput}${MissingError}" "FreeType PCF driver notice" MissingNoticeDiagnostic)
if(MissingNoticeDiagnostic EQUAL -1)
	message(FATAL_ERROR "Missing FreeType source did not produce an actionable error: ${MissingOutput}${MissingError}")
endif()

set(OutsidePlayer "${PF_TEST_ROOT}/outside-player.exe")
file(WRITE "${OutsidePlayer}" "outside executable")
RunTemplate("${ValidRoot}" "${OutsidePlayer}" "${ResourceRoot}" "${Descriptor}"
	"${PF_TEST_SOURCE_ROOT}/PulseForge/Vendor/freetype" OutsideResult OutsideOutput OutsideError)
if(OutsideResult EQUAL 0)
	message(FATAL_ERROR "Template generation accepted an executable outside its root")
endif()
string(FIND "${OutsideOutput}${OutsideError}" "outside its template root" OutsideDiagnostic)
if(OutsideDiagnostic EQUAL -1)
	message(FATAL_ERROR "Executable containment failure was not diagnosed: ${OutsideOutput}${OutsideError}")
endif()

set(OutsideResource "${PF_TEST_ROOT}/outside resources")
file(MAKE_DIRECTORY "${OutsideResource}/Shaders")
RunTemplate("${ValidRoot}" "${ValidRoot}/bin/PulseForge.Player.v1.exe" "${OutsideResource}" "${Descriptor}"
	"${PF_TEST_SOURCE_ROOT}/PulseForge/Vendor/freetype" OutsideResourceResult OutsideResourceOutput OutsideResourceError)
if(OutsideResourceResult EQUAL 0)
	message(FATAL_ERROR "Template generation accepted notices/resources outside its root")
endif()
string(FIND "${OutsideResourceOutput}${OutsideResourceError}" "outside the player template root" ResourceDiagnostic)
if(ResourceDiagnostic EQUAL -1)
	message(FATAL_ERROR "Resource containment failure was not diagnosed: ${OutsideResourceOutput}${OutsideResourceError}")
endif()
