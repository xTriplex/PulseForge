#include "Editor/EditorAssetSelector.h"

#include "Editor/EditorAssetSelectorPolicy.h"

#include <cfloat>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
		}

	} // namespace

	bool EditorAssetSelector::Draw(const char* Label, std::optional<PulseForge::AssetID>& Current,
		const std::vector<PulseForge::AssetRecord>& Assets, std::initializer_list<std::string_view> Extensions, bool AllowNone)
	{
		std::string Preview = "<None>";
		std::string PreviewPath;
		if (Current)
		{
			const auto Record = std::find_if(
				Assets.begin(), Assets.end(), [&](const PulseForge::AssetRecord& Candidate) { return Candidate.ID == *Current; });
			if (Record != Assets.end())
			{
				PreviewPath = PathToUtf8(Record->ProjectRelativePath);
				Preview = PathToUtf8(Record->ProjectRelativePath.filename());
			}
			else Preview = "<Missing asset: " + Current->ToString() + ">";
		}

		bool Changed = false;
		const bool IsOpen = ImGui::BeginCombo(Label, Preview.c_str());
		if (!PreviewPath.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PreviewPath.c_str());
		if (!IsOpen) return false;

		const bool Activated = ImGui::IsItemActivated();
		if (Activated || m_SearchOwner != Label)
		{
			m_Search.fill('\0');
			m_SearchOwner = Label;
		}
		if (Activated) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint(
			"##AssetSelectorSearch", "Search compatible assets by name or folder...", m_Search.data(), m_Search.size());

		if (AllowNone)
		{
			const bool IsSelected = !Current;
			if (ImGui::Selectable("None", IsSelected))
			{
				Current.reset();
				Changed = true;
			}
			if (IsSelected) ImGui::SetItemDefaultFocus();
		}

		bool HasCandidates = false;
		bool HasVisibleCandidates = false;
		for (const PulseForge::AssetRecord& Asset : Assets)
		{
			if (!AssetHasExtension(Asset, Extensions)) continue;
			HasCandidates = true;
			const std::string Identifier = Asset.ID.ToString();
			const std::string Path = PathToUtf8(Asset.ProjectRelativePath);
			if (!AssetSelectorMatchesSearch(Path, m_Search.data())) continue;
			HasVisibleCandidates = true;
			const bool IsSelected = Current && *Current == Asset.ID;
			ImGui::PushID(Identifier.c_str());
			if (ImGui::Selectable(Path.c_str(), IsSelected))
			{
				Current = Asset.ID;
				Changed = true;
			}
			if (IsSelected) ImGui::SetItemDefaultFocus();
			ImGui::PopID();
		}
		if (!HasCandidates) ImGui::TextDisabled("No compatible managed assets are available.");
		else if (!HasVisibleCandidates) ImGui::TextDisabled("No compatible assets match this search.");

		ImGui::EndCombo();
		return Changed;
	}
} // namespace PulseForgeEditor
