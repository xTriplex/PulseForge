#define _GNU_SOURCE

#include "Editor/Detail/EditorPlayerProcessPosix.h"

namespace PulseForgeEditor
{
	std::expected<std::unique_ptr<EditorPlayerProcessImpl>, std::string> CreatePlatformEditorPlayerProcess(
		const EditorPlayerLaunchSpec& Spec)
	{
		return Detail::CreatePosixEditorPlayerProcess(Spec);
	}
} // namespace PulseForgeEditor
