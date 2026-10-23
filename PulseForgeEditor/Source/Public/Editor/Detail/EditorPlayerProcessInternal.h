#pragma once

#include "Editor/EditorPlayerProcess.h"

namespace PulseForgeEditor
{
	class EditorPlayerProcessImpl
	{
	public:
		virtual ~EditorPlayerProcessImpl() = default;
		[[nodiscard]] virtual EditorPlayerProcessUpdate Poll(size_t MaximumOutputBytes) = 0;
		virtual void RequestStop() noexcept = 0;
	};

	[[nodiscard]] std::expected<std::unique_ptr<EditorPlayerProcessImpl>, std::string> CreatePlatformEditorPlayerProcess(
		const EditorPlayerLaunchSpec& Spec);
} // namespace PulseForgeEditor
