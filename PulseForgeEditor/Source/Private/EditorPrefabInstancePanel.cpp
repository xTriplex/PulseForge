#include "Editor/EditorPrefabInstancePanel.h"

#include "Editor/EditorSceneWorkspaceState.h"
#include "Editor/EditorStyle.h"

#include <imgui.h>

#include <string>
#include <string_view>
#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		[[nodiscard]] std::string MakePlanSummaryText(const PulseForge::PrefabReconcileSummary& Summary)
		{
			return "Added " + std::to_string(Summary.Added) + "   Existing processed " +
				std::to_string(Summary.ExistingEntitiesProcessed) + "   Removed " + std::to_string(Summary.Removed) +
				"   Conflicts " + std::to_string(Summary.ConflictCount);
		}
	}

	void EditorPrefabInstancePanel::Draw(
		PulseForge::Scene* SceneValue,
		EditorSceneWorkspaceState& Workspace,
		EditorStyle& Style,
		EditorPrefabInstancePanelActions& Actions)
	{
		const auto& Selection = Workspace.Selection();
		if (!SceneValue || Selection.GetEntities().size() != 1 || !Selection.GetPrimary())
		{
			m_ErrorEntity.reset();
			m_Error.clear();
			m_Status.clear();
			return;
		}

		const PulseForge::UUID SelectedIdentifier = *Selection.GetPrimary();
		if (m_ErrorEntity && *m_ErrorEntity != SelectedIdentifier)
		{
			m_Error.clear();
			m_Status.clear();
		}
		m_ErrorEntity = SelectedIdentifier;

		const auto SelectedEntity = SceneValue->FindEntity(SelectedIdentifier);
		if (!SelectedEntity)
		{
			m_Error.clear();
			return;
		}

		auto CurrentStatus = Actions.GetStatus(*SelectedEntity);
		if (!CurrentStatus)
		{
			m_Error = CurrentStatus.error();
			if (Style.SectionHeader(EditorIcon::Prefab, "Linked Prefab Instance", true))
				ImGui::TextWrapped("Could not read linked prefab state: %s", m_Error.c_str());
			return;
		}
		if (!CurrentStatus->has_value())
		{
			m_Error.clear();
			return;
		}

		EditorPrefabInstancePanelStatus Status = std::move(**CurrentStatus);
		if (Style.SectionHeader(EditorIcon::Prefab, "Linked Prefab Instance", true))
		{
			ImGui::TextWrapped("Source asset  %s", Status.SourceAsset.ToString().c_str());
			ImGui::TextWrapped("Instance  %s", Status.InstanceID.ToString().c_str());
			ImGui::Text("Overrides  %zu    Conflicts  %zu", Status.OverrideCount, Status.ConflictCount);

			if (Style.SmallButton(EditorIcon::Refresh, "Plan Update"))
			{
				auto Planned = Actions.PlanUpdate(*SelectedEntity);
				if (Planned)
				{
					Status = std::move(*Planned);
					m_Error.clear();
				}
				else m_Error = Planned.error();
			}

			if (Status.HasPendingPlan)
			{
				ImGui::SameLine();
				const bool CanRecordConflicts = !Status.CanApply && Status.ConflictCount > 0;
				ImGui::BeginDisabled(!Status.CanApply && !CanRecordConflicts);
				const std::string_view ButtonLabel = Status.CanApply ? "Apply Update" : "Record Conflicts";
				if (Style.AccentButton(EditorIcon::Refresh, ButtonLabel))
				{
					auto Applied = Actions.ApplyUpdate(*SelectedEntity);
					if (Applied)
					{
						Status = std::move(*Applied);
						m_Error.clear();
					}
					else m_Error = Applied.error();
				}
				ImGui::EndDisabled();
			}

			const bool HasPlanSummary = Status.HasPendingPlan || Status.PlanSummary.Added > 0 ||
				Status.PlanSummary.ExistingEntitiesProcessed > 0 || Status.PlanSummary.Removed > 0 ||
				Status.PlanSummary.ConflictCount > 0;
			if (HasPlanSummary)
			{
				ImGui::Spacing();
				Style.TextMuted(MakePlanSummaryText(Status.PlanSummary));
			}
			DrawConflicts(Status.Conflicts);

			ImGui::SeparatorText("Override Operations");
			ImGui::TextWrapped("Apply stages supported source-bound changes in the prefab document draft. Save that prefab document to write them; the linked instance keeps its override intent.");
			ImGui::BeginDisabled(!Status.HasSelectedSourceOverrides);
			if (Style.SmallButton(EditorIcon::Save, "Apply Selected to Source Draft"))
			{
				auto Result = Actions.ApplyOverrides(*SelectedEntity, false);
				if (Result) { m_Status = "Queued selected overrides for the prefab source draft."; m_Error.clear(); }
				else m_Error = Result.error();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(Status.OverrideCount == 0);
			if (Style.SmallButton(EditorIcon::Save, "Apply All to Source Draft"))
			{
				auto Result = Actions.ApplyOverrides(*SelectedEntity, true);
				if (Result) { m_Status = "Queued all applicable overrides for the prefab source draft."; m_Error.clear(); }
				else m_Error = Result.error();
			}
			ImGui::EndDisabled();
			ImGui::BeginDisabled(!Status.HasSelectedSourceOverrides);
			if (Style.SmallButton(EditorIcon::Clear, "Revert Selected"))
			{
				auto Result = Actions.RevertOverrides(*SelectedEntity, false);
				if (Result) { m_Status = "Queued selected overrides for reversion to the source draft."; m_Error.clear(); }
				else m_Error = Result.error();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(Status.OverrideCount == 0);
			if (Style.SmallButton(EditorIcon::Clear, "Revert All"))
			{
				auto Result = Actions.RevertOverrides(*SelectedEntity, true);
				if (Result) { m_Status = "Queued all overrides for reversion to the source draft."; m_Error.clear(); }
				else m_Error = Result.error();
			}
			ImGui::EndDisabled();

			if (!m_Error.empty())
			{
				ImGui::Spacing();
				ImGui::TextWrapped("Prefab operation failed: %s", m_Error.c_str());
			}
			else if (!m_Status.empty())
			{
				ImGui::Spacing();
				Style.TextMuted(m_Status);
			}
		}
	}

	void EditorPrefabInstancePanel::DrawConflicts(const std::vector<PulseForge::PrefabConflictRecord>& Conflicts) const
	{
		for (const PulseForge::PrefabConflictRecord& Conflict : Conflicts)
		{
			if (!Conflict.Message.empty()) ImGui::BulletText("%s", Conflict.Message.c_str());
			else ImGui::BulletText("Prefab conflict for entity %s", Conflict.Entity.Key.ToString().c_str());
		}
	}
} // namespace PulseForgeEditor
