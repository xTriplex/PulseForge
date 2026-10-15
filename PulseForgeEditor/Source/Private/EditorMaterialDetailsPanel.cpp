#include "Editor/EditorMaterialDetailsPanel.h"

#include "Assets/Project.h"
#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorMaterialEditorPolicy.h"
#include "Editor/EditorMaterialDraftController.h"
#include "Editor/EditorMaterialPropertyLayoutPolicy.h"
#include "Editor/EditorStyle.h"

#include <cfloat>
#include <cctype>
#include <filesystem>

namespace PulseForgeEditor
{
	namespace
	{
		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return std::string(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
		}

		std::string NormalizedExtension(const std::filesystem::path& Path)
		{
			std::string Extension = Path.extension().string();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(),
				[](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
			return Extension;
		}
	} // namespace

	void EditorMaterialDetailsPanel::DrawContents(const PulseForge::Project* ProjectValue,
		std::optional<PulseForge::AssetID>& SelectedAsset, const std::vector<PulseForge::AssetRecord>& Assets,
		EditorMaterialDraftController& Drafts, EditorAssetSelector& AssetSelector, EditorStyle& Style,
		EditorMaterialDetailsActionSink& Actions)
	{
		if (!ProjectValue)
		{
			Style.EmptyState(EditorIcon::Material, "Open a project to inspect a material.");
			return;
		}
		if (!SelectedAsset)
		{
			Style.EmptyState(EditorIcon::Material, "Show Material Details from a material in the Content Browser.");
			return;
		}

		const auto Asset = ProjectValue->GetAssetRegistry().Find(*SelectedAsset);
		if (!Asset || NormalizedExtension(Asset->ProjectRelativePath) != ".material")
		{
			SelectedAsset.reset();
			Style.EmptyState(EditorIcon::Warning, "The material is no longer registered. Refresh the Content Browser and select it again.");
			return;
		}

		const std::string Name = PathToUtf8(Asset->ProjectRelativePath.filename());
		const std::string FullPath = PathToUtf8(Asset->ProjectRelativePath);
		const std::string Identifier = Asset->ID.ToString();
		Style.IconText(EditorIcon::Material, Name);
		if (Drafts.IsDirty(*ProjectValue, Asset->ID))
		{
			ImGui::SameLine();
			ImGui::TextColored(Style.GetColor(EditorColorToken::Warning), "Unsaved");
		}
		ImGui::TextWrapped("%s", FullPath.c_str());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nUUID: %s", FullPath.c_str(), Identifier.c_str());
		ImGui::Separator();

		const bool StackActions = ImGui::GetContentRegionAvail().x < 320.0f;
		const float FooterHeight = ImGui::GetFrameHeightWithSpacing() * (StackActions ? 3.0f : 2.0f) + 8.0f;
		const float PropertiesHeight = (std::max)(ImGui::GetContentRegionAvail().y - FooterHeight, 0.0f);
		if (ImGui::BeginChild("MaterialDetailsProperties", ImVec2(0.0f, PropertiesHeight), ImGuiChildFlags_None,
				ImGuiWindowFlags_AlwaysVerticalScrollbar))
			(void)DrawMaterialProperties(*ProjectValue, *Asset, Assets, Drafts, AssetSelector, Style, Actions);
		ImGui::EndChild();
		ImGui::Separator();
		DrawMaterialActions(*Asset, *ProjectValue, Drafts, Style, Actions);
	}

	bool EditorMaterialDetailsPanel::DrawMaterialProperties(const PulseForge::Project& ProjectValue, const PulseForge::AssetRecord& Asset,
		const std::vector<PulseForge::AssetRecord>& Assets, EditorMaterialDraftController& Drafts, EditorAssetSelector& AssetSelector,
		EditorStyle& Style, EditorMaterialDetailsActionSink& Actions)
	{
		if (const std::string* Error = Drafts.FindError(ProjectValue, Asset.ID))
		{
			ImGui::TextColored(Style.GetColor(EditorColorToken::Error), "Material could not be loaded: %s", Error->c_str());
			if (ImGui::SmallButton("Retry Material Load")) Drafts.ClearLoadError(ProjectValue, Asset.ID);
			return false;
		}

		auto Loaded = Drafts.LoadDraft(ProjectValue, Asset.ID);
		if (!Loaded)
		{
			ImGui::TextColored(Style.GetColor(EditorColorToken::Error), "Material could not be loaded: %s", Loaded.error().c_str());
			if (ImGui::SmallButton("Retry Material Load")) Drafts.ClearLoadError(ProjectValue, Asset.ID);
			return false;
		}

		PulseForge::MaterialAssetDesc* Draft = &Loaded->get();
		const PulseForge::MaterialAssetDesc MaterialBeforeEdit = *Draft;
		PulseForge::MaterialAssetDesc& Material = *Draft;
		bool Changed = false;
		bool UseTwoColumnLayout = GetMaterialPropertyLayout(ImGui::GetContentRegionAvail().x) == MaterialPropertyLayout::TwoColumn;
		bool PropertyTableOpen = false;
		if (UseTwoColumnLayout)
		{
			ImGui::PushStyleColor(ImGuiCol_TableBorderLight, Style.GetColor(EditorColorToken::Border));
			PropertyTableOpen = ImGui::BeginTable("MaterialAssetProperties", 2,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings);
			if (PropertyTableOpen)
			{
				const float AvailableWidth = ImGui::GetContentRegionAvail().x;
				ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, GetMaterialPropertyLabelWidth(AvailableWidth));
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			}
			else
			{
				ImGui::PopStyleColor();
				UseTwoColumnLayout = false;
			}
		}

		const auto BeginMaterialPropertyRow = [&Style, UseTwoColumnLayout](std::string_view Label)
		{
			if (UseTwoColumnLayout) return Style.BeginPropertyRow(Label);

			Style.TextMuted(Label);
			const float LabelWidth = ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x;
			if (LabelWidth > ImGui::GetContentRegionAvail().x && ImGui::IsItemHovered())
				ImGui::SetTooltip("%.*s", static_cast<int>(Label.size()), Label.data());
			return true;
		};
		const auto DrawMaterialPropertyRow = [UseTwoColumnLayout, &BeginMaterialPropertyRow](std::string_view Label, auto&& DrawValue)
		{
			if (!BeginMaterialPropertyRow(Label)) return;
			DrawValue();
			if (!UseTwoColumnLayout) ImGui::Spacing();
		};

		if (!UseTwoColumnLayout || PropertyTableOpen)
		{
			DrawMaterialPropertyRow("Base Color Factor",
				[&]
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					Changed |= ImGui::DragFloat4("##MaterialBaseColorFactor", &Material.BaseColorFactor.x, 0.01f, 0.0f, 1.0f, "%.3f");
				});
			DrawMaterialPropertyRow("Alpha Mode",
				[&]
				{
					int AlphaMode = static_cast<int>(Material.AlphaMode);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (ImGui::Combo("##MaterialAlphaMode", &AlphaMode, "Opaque\0Mask\0Blend\0"))
					{
						Material.AlphaMode = static_cast<PulseForge::MaterialAlphaMode>(AlphaMode);
						Changed = true;
					}
				});
			if (Material.AlphaMode == PulseForge::MaterialAlphaMode::Mask)
				DrawMaterialPropertyRow("Alpha Cutoff",
					[&]
					{
						ImGui::SetNextItemWidth(-FLT_MIN);
						Changed |= ImGui::DragFloat("##MaterialAlphaCutoff", &Material.AlphaCutoff, 0.01f, 0.0f, 1.0f, "%.3f");
					});
			DrawMaterialPropertyRow("Double Sided", [&] { Changed |= ImGui::Checkbox("##MaterialDoubleSided", &Material.DoubleSided); });
			DrawMaterialPropertyRow("Base Color Texture",
				[&]
				{
					std::optional<PulseForge::AssetID> Texture =
						Material.BaseColorTexture.IsNil() ? std::nullopt : std::optional<PulseForge::AssetID>(Material.BaseColorTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (AssetSelector.Draw(
							"##MaterialBaseColorTexture", Texture, Assets, { ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.BaseColorTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
			DrawMaterialPropertyRow("Metallic / Roughness",
				[&]
				{
					const float AvailableWidth = ImGui::GetContentRegionAvail().x;
					const float ItemSpacing = ImGui::GetStyle().ItemSpacing.x;
					const MaterialValuePairLayout PairLayout = GetMaterialValuePairLayout(AvailableWidth, ItemSpacing);
					ImGui::SetNextItemWidth(PairLayout.FirstWidth);
					Changed |= ImGui::DragFloat("##MaterialMetallic", &Material.MetallicFactor, 0.01f, 0.0f, 1.0f, "M %.3f");
					if (!PairLayout.Stacked) ImGui::SameLine(0.0f, ItemSpacing);
					ImGui::SetNextItemWidth(PairLayout.SecondWidth);
					Changed |= ImGui::DragFloat("##MaterialRoughness", &Material.RoughnessFactor, 0.01f, 0.0f, 1.0f, "R %.3f");
				});
			DrawMaterialPropertyRow("Metallic-Roughness Map",
				[&]
				{
					std::optional<PulseForge::AssetID> Texture =
						Material.MetallicRoughnessTexture.IsNil() ? std::nullopt
																  : std::optional<PulseForge::AssetID>(Material.MetallicRoughnessTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (AssetSelector.Draw(
							"##MaterialMetallicRoughnessTexture", Texture, Assets, { ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.MetallicRoughnessTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
			DrawMaterialPropertyRow("Normal Map",
				[&]
				{
					std::optional<PulseForge::AssetID> Texture =
						Material.NormalTexture.IsNil() ? std::nullopt : std::optional<PulseForge::AssetID>(Material.NormalTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (AssetSelector.Draw("##MaterialNormalTexture", Texture, Assets, { ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.NormalTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
			DrawMaterialPropertyRow("Normal Scale",
				[&]
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					Changed |= ImGui::DragFloat("##MaterialNormalScale", &Material.NormalScale, 0.02f, 0.0f, 4.0f, "%.3f");
				});
			DrawMaterialPropertyRow("Occlusion Map",
				[&]
				{
					std::optional<PulseForge::AssetID> Texture =
						Material.OcclusionTexture.IsNil() ? std::nullopt : std::optional<PulseForge::AssetID>(Material.OcclusionTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (AssetSelector.Draw(
							"##MaterialOcclusionTexture", Texture, Assets, { ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.OcclusionTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
			DrawMaterialPropertyRow("Occlusion Strength",
				[&]
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					Changed |= ImGui::DragFloat("##MaterialOcclusionStrength", &Material.OcclusionStrength, 0.01f, 0.0f, 1.0f, "%.3f");
				});
			DrawMaterialPropertyRow("Emissive Texture",
				[&]
				{
					std::optional<PulseForge::AssetID> Texture =
						Material.EmissiveTexture.IsNil() ? std::nullopt : std::optional<PulseForge::AssetID>(Material.EmissiveTexture);
					ImGui::SetNextItemWidth(-FLT_MIN);
					if (AssetSelector.Draw("##MaterialEmissiveTexture", Texture, Assets, { ".png", ".jpg", ".jpeg", ".bmp", ".ppm" }, true))
					{
						Material.EmissiveTexture = Texture.value_or(PulseForge::AssetID{});
						Changed = true;
					}
				});
			DrawMaterialPropertyRow("Emissive Factor (Linear)",
				[&]
				{
					ImGui::SetNextItemWidth(-FLT_MIN);
					Changed |= ImGui::DragFloat3("##MaterialEmissiveFactor", &Material.EmissiveFactor.x, 0.05f, 0.0f, 1000.0f, "%.3f");
				});
		}
		if (PropertyTableOpen) Style.EndPropertyTable();

		ImGui::PushStyleColor(ImGuiCol_Text, Style.GetColor(EditorColorToken::TextMuted));
		ImGui::TextWrapped("Base color and emissive maps use sRGB; normal, metallic-roughness and occlusion maps use linear data.");
		ImGui::TextWrapped(
			"Mask clips below Alpha Cutoff and casts cutout shadows. Blend is sorted back-to-front and does not cast shadows.");
		ImGui::PopStyleColor();

		if (Changed && Drafts.BeginEdit(ProjectValue, Asset.ID, MaterialBeforeEdit))
			Actions.MaterialDetailsDraftEdited(ProjectValue.GetDescription().ID, Asset.ID);
		return true;
	}

	void EditorMaterialDetailsPanel::DrawMaterialActions(const PulseForge::AssetRecord& Asset, const PulseForge::Project& ProjectValue,
		EditorMaterialDraftController& Drafts, EditorStyle& Style, EditorMaterialDetailsActionSink& Actions)
	{
		const bool HasDraft = Drafts.ContainsDraft(ProjectValue, Asset.ID);
		const bool IsDirty = Drafts.IsDirty(ProjectValue, Asset.ID);
		if (IsDirty) ImGui::TextColored(Style.GetColor(EditorColorToken::Warning), "Unsaved material draft");
		else Style.TextMuted("No unsaved changes");

		const float AvailableWidth = ImGui::GetContentRegionAvail().x;
		const bool StackButtons = AvailableWidth < 320.0f;
		const float ButtonWidth = StackButtons ? -1.0f : (AvailableWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		ImGui::BeginDisabled(!HasDraft || !IsDirty);
		if (ImGui::Button("Save Material", ImVec2(ButtonWidth, 0.0f))) (void)Actions.SaveMaterialFromDetails(Asset.ID);
		if (StackButtons)
		{
			if (ImGui::Button("Revert Material", ImVec2(-1.0f, 0.0f))) (void)Actions.RevertMaterialFromDetails(Asset.ID);
		}
		else
		{
			ImGui::SameLine();
			if (ImGui::Button("Revert Material", ImVec2(ButtonWidth, 0.0f))) (void)Actions.RevertMaterialFromDetails(Asset.ID);
		}
		ImGui::EndDisabled();
	}
} // namespace PulseForgeEditor
