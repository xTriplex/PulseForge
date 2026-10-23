#include "Editor/EditorPlayerProcess.h"

#include "Editor/Detail/EditorPlayerProcessInternal.h"

#include <algorithm>
#include <system_error>
#include <string_view>
#include <utility>

namespace PulseForgeEditor
{
	EditorPlayerProcess::EditorPlayerProcess(std::unique_ptr<EditorPlayerProcessImpl> Implementation) noexcept
		: m_Impl(std::move(Implementation))
	{
	}

	EditorPlayerProcess::~EditorPlayerProcess()
	{
		RequestStop();
	}

	std::expected<std::unique_ptr<EditorPlayerProcess>, std::string> EditorPlayerProcess::Launch(const EditorPlayerLaunchSpec& Spec)
	{
		if (Spec.Executable.empty() || !Spec.Executable.is_absolute())
			return std::unexpected("The player executable path must be absolute and non-empty");
		if (Spec.WorkingDirectory.empty() || !Spec.WorkingDirectory.is_absolute())
			return std::unexpected("The player working directory must be absolute and non-empty");
		if (Spec.Executable.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos ||
			Spec.WorkingDirectory.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
			return std::unexpected("The player executable and working directory cannot contain NUL characters");
		for (const std::string& Argument : Spec.Arguments)
			if (Argument.find('\0') != std::string::npos) return std::unexpected("Player arguments cannot contain NUL characters");

		std::error_code Error;
		if (!std::filesystem::is_regular_file(Spec.Executable, Error) || Error)
			return std::unexpected("The player executable is unavailable");
		if (!std::filesystem::is_directory(Spec.WorkingDirectory, Error) || Error)
			return std::unexpected("The player working directory is unavailable");

		auto Implementation = CreatePlatformEditorPlayerProcess(Spec);
		if (!Implementation) return std::unexpected(Implementation.error());
		return std::unique_ptr<EditorPlayerProcess>(new EditorPlayerProcess(std::move(*Implementation)));
	}

	EditorPlayerProcessUpdate EditorPlayerProcess::Poll(size_t MaximumOutputBytes)
	{
		if (!m_Impl) return { .Error = "The player process implementation is unavailable" };
		EditorPlayerProcessUpdate Update = m_Impl->Poll((std::min)(MaximumOutputBytes, size_t{ 1024 * 1024 }));
		constexpr std::string_view Acknowledgement = "PULSEFORGE_EDITOR_CONTROL_ACCEPTED";
		m_ControlAcknowledgementTail.append(Update.OutputChunk);
		if (m_ControlAcknowledgementTail.find(Acknowledgement) != std::string::npos)
			m_CooperativeControlAcknowledged = true;
		if (m_ControlAcknowledgementTail.size() >= Acknowledgement.size())
			m_ControlAcknowledgementTail.erase(0, m_ControlAcknowledgementTail.size() - Acknowledgement.size() + 1);
		Update.CooperativeStopObserved = m_StopRequested && m_CooperativeControlAcknowledged && Update.ExitCode == 0 &&
			!Update.ForcedTermination;
		return Update;
	}

	void EditorPlayerProcess::RequestStop() noexcept
	{
		if (!m_Impl || m_StopRequested) return;
		m_StopRequested = true;
		m_Impl->RequestStop();
	}
} // namespace PulseForgeEditor
