#include "Editor/EditorMaterialDocumentEditor.h"

#include "Assets/AssetTypes.h"
#include "Assets/Project.h"
#include "Editor/EditorAssetSelector.h"
#include "Editor/EditorMaterialDetailsPanel.h"
#include "Editor/EditorMaterialDraftController.h"
#include "Editor/EditorStyle.h"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <algorithm>
#include <bit>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace PulseForgeEditor
{
	namespace NodeEditor = ax::NodeEditor;

	struct EditorMaterialDocumentEditor::State final : EditorDocumentEditorState
	{
		~State() override
		{
			if (!Canvas) return;
			auto* const Previous = NodeEditor::GetCurrentEditor();
			NodeEditor::SetCurrentEditor(Canvas);
			NodeEditor::DestroyEditor(Canvas);
			NodeEditor::SetCurrentEditor(Previous == Canvas ? nullptr : Previous);
		}
		NodeEditor::EditorContext* Canvas = nullptr;
		std::unordered_set<uintptr_t> PositionedNodes;
		std::string Error;
	};

	namespace
	{
		struct MaterialGraphPinHandle final
		{
			PulseForge::UUID Node;
			bool Output = false;
			uint8_t Slot = 0;
		};

		uintptr_t MaterialGraphCanvasHandle(PulseForge::UUID ID, uint64_t Salt) noexcept
		{
			uint64_t Value = ID.GetHigh() ^ std::rotl(ID.GetLow(), 23) ^ (Salt * 0x9e3779b97f4a7c15ull);
			Value ^= Value >> 30;
			Value *= 0xbf58476d1ce4e5b9ull;
			Value ^= Value >> 27;
			Value *= 0x94d049bb133111ebull;
			Value ^= Value >> 31;
			return static_cast<uintptr_t>(Value == 0 ? 1 : Value);
		}

		const char* MaterialGraphNodeLabel(PulseForge::MaterialGraphNodeKind Kind)
		{
			switch (Kind)
			{
			case PulseForge::MaterialGraphNodeKind::FloatConstant: return "Constant";
			case PulseForge::MaterialGraphNodeKind::ColorConstant: return "Color";
			case PulseForge::MaterialGraphNodeKind::FloatParameter: return "Scalar Parameter";
			case PulseForge::MaterialGraphNodeKind::Add: return "Add";
			case PulseForge::MaterialGraphNodeKind::Multiply: return "Multiply";
			case PulseForge::MaterialGraphNodeKind::Reroute: return "Reroute";
			case PulseForge::MaterialGraphNodeKind::PbrOutput: return "PBR Output";
			}
			return "Unknown";
		}

		bool IsOnlyUnconnectedMaterialInputs(const PulseForge::MaterialGraphValidation& Validation)
		{
			return std::all_of(Validation.Diagnostics.begin(), Validation.Diagnostics.end(), [](const auto& Diagnostic)
				{ return Diagnostic.Message == "Required material graph input is unconnected"; });
		}

		template<typename StateType>
		void DrawMaterialGraphCanvas(StateType& State, EditorDocumentSession& Session, PulseForge::Project& Project,
			PulseForge::MaterialAssetDesc& Draft, EditorMaterialDraftController& Drafts,
			EditorDocumentManager& Documents)
		{
			if (!Draft.Graph) return;
			if (!State.Canvas)
			{
				NodeEditor::Config Config;
				Config.SettingsFile = nullptr;
				State.Canvas = NodeEditor::CreateEditor(&Config);
				if (!State.Canvas)
				{
					State.Error = "Could not create the material graph canvas.";
					return;
				}
			}

			auto* const Previous = NodeEditor::GetCurrentEditor();
			NodeEditor::SetCurrentEditor(State.Canvas);
			std::unordered_map<uintptr_t, MaterialGraphPinHandle> Pins;
			std::unordered_map<uintptr_t, PulseForge::UUID> Connections;
			std::vector<std::pair<PulseForge::UUID, ImVec2>> UpdatedPositions;
			std::unordered_set<uintptr_t> ActiveNodeHandles;
			const auto PinHandle = [&](PulseForge::UUID Node, bool Output, uint8_t Slot)
			{
				const uintptr_t Handle = MaterialGraphCanvasHandle(Node,
					(Output ? 0x100ull : 0x200ull) + Slot);
				Pins.emplace(Handle, MaterialGraphPinHandle{ Node, Output, Slot });
				return Handle;
			};

			ImGui::TextDisabled("Edit scalar or color graphs. Scalar output links replicate to RGB; color links feed PBR base color.");
			const auto AddNode = [&](PulseForge::MaterialGraphNodeKind Kind, const char* Label)
			{
				if (!ImGui::Button(Label)) return;
				const auto NodeID = PulseForge::UUID::Generate();
				if (!NodeID)
				{
					State.Error = NodeID.error().Message;
					return;
				}
				const auto Before = Draft;
				const float Column = static_cast<float>(Draft.Graph->Nodes.size() % 4);
				const float Row = static_cast<float>(Draft.Graph->Nodes.size() / 4);
				std::string ParameterName;
				if (Kind == PulseForge::MaterialGraphNodeKind::FloatParameter)
					ParameterName = "Parameter" + std::to_string(Draft.Graph->Nodes.size());
				Draft.Graph->Nodes.push_back({ *NodeID, Kind, 0.0f, std::move(ParameterName),
					80.0 + Column * 220.0, 80.0 + Row * 140.0 });
				if (Drafts.BeginEdit(Project, Session.Identity.Asset, Before))
					(void)Documents.MarkEdited(Session.Identity);
				State.Error.clear();
			};
			AddNode(PulseForge::MaterialGraphNodeKind::FloatConstant, "Add Constant"); ImGui::SameLine();
			AddNode(PulseForge::MaterialGraphNodeKind::ColorConstant, "Add Color"); ImGui::SameLine();
			AddNode(PulseForge::MaterialGraphNodeKind::FloatParameter, "Add Parameter"); ImGui::SameLine();
			AddNode(PulseForge::MaterialGraphNodeKind::Add, "Add"); ImGui::SameLine();
			AddNode(PulseForge::MaterialGraphNodeKind::Multiply, "Multiply"); ImGui::SameLine();
			AddNode(PulseForge::MaterialGraphNodeKind::Reroute, "Reroute");

			const auto Available = ImGui::GetContentRegionAvail();
			NodeEditor::Begin("##MaterialGraphCanvas", ImVec2(Available.x, 360.0f));
			for (auto& Node : Draft.Graph->Nodes)
			{
				const uintptr_t NodeHandle = MaterialGraphCanvasHandle(Node.ID, 0x10ull);
				ActiveNodeHandles.insert(NodeHandle);
				const NodeEditor::NodeId CanvasNode(NodeHandle);
				if (State.PositionedNodes.insert(NodeHandle).second)
					NodeEditor::SetNodePosition(CanvasNode, ImVec2(static_cast<float>(Node.X), static_cast<float>(Node.Y)));
				NodeEditor::BeginNode(CanvasNode);
				ImGui::Text("%s", MaterialGraphNodeLabel(Node.Kind));
				ImGui::TextDisabled("%s", Node.ID.ToString().substr(0, 8).c_str());
				const bool HasInput = Node.Kind == PulseForge::MaterialGraphNodeKind::Add ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::Multiply ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::Reroute ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::PbrOutput;
				const bool HasOutput = Node.Kind == PulseForge::MaterialGraphNodeKind::FloatConstant ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::ColorConstant ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::FloatParameter ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::Add ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::Multiply ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::Reroute;
				if (HasInput)
				{
					const uint8_t InputCount = Node.Kind == PulseForge::MaterialGraphNodeKind::Add ||
						Node.Kind == PulseForge::MaterialGraphNodeKind::Multiply ? 2 : 1;
					for (uint8_t Slot = 0; Slot < InputCount; ++Slot)
					{
						const uintptr_t Handle = PinHandle(Node.ID, false, Slot);
						NodeEditor::BeginPin(NodeEditor::PinId(Handle), NodeEditor::PinKind::Input);
						ImGui::Text("In %u", static_cast<unsigned>(Slot));
						NodeEditor::EndPin();
					}
				}
				if (Node.Kind == PulseForge::MaterialGraphNodeKind::FloatConstant ||
					Node.Kind == PulseForge::MaterialGraphNodeKind::FloatParameter)
				{
					float Value = Node.Value;
					if (ImGui::DragFloat("Value", &Value, 0.01f))
					{
						const auto Before = Draft;
						Node.Value = Value;
						if (Drafts.BeginEdit(Project, Session.Identity.Asset, Before))
							(void)Documents.MarkEdited(Session.Identity);
					}
				}
				if (Node.Kind == PulseForge::MaterialGraphNodeKind::ColorConstant)
				{
					float Color[3]{ Node.Color[0], Node.Color[1], Node.Color[2] };
					if (ImGui::ColorEdit3("RGB", Color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
					{
						const auto Before = Draft;
						Node.Color = { Color[0], Color[1], Color[2] };
						if (Drafts.BeginEdit(Project, Session.Identity.Asset, Before))
							(void)Documents.MarkEdited(Session.Identity);
					}
				}
				if (HasOutput)
				{
					const uintptr_t Handle = PinHandle(Node.ID, true, 0);
					NodeEditor::BeginPin(NodeEditor::PinId(Handle), NodeEditor::PinKind::Output);
					ImGui::Text("%s", Node.Kind == PulseForge::MaterialGraphNodeKind::Reroute ? "Out" : "Value");
					NodeEditor::EndPin();
				}
				NodeEditor::EndNode();
				const ImVec2 Position = NodeEditor::GetNodePosition(CanvasNode);
				if (std::abs(Position.x - Node.X) > 0.5f || std::abs(Position.y - Node.Y) > 0.5f)
					UpdatedPositions.emplace_back(Node.ID, Position);
			}
			std::erase_if(State.PositionedNodes, [&](uintptr_t Handle) { return !ActiveNodeHandles.contains(Handle); });
			for (const auto& Connection : Draft.Graph->Connections)
			{
				const uintptr_t LinkHandle = MaterialGraphCanvasHandle(Connection.ID, 0x30ull);
				const uintptr_t Output = PinHandle(Connection.OutputNode, true, 0);
				const uintptr_t Input = PinHandle(Connection.InputNode, false, Connection.InputSlot);
				Connections.emplace(LinkHandle, Connection.ID);
				NodeEditor::Link(NodeEditor::LinkId(LinkHandle), NodeEditor::PinId(Output), NodeEditor::PinId(Input));
			}

			if (NodeEditor::BeginCreate())
			{
				NodeEditor::PinId First;
				NodeEditor::PinId Second;
				while (NodeEditor::QueryNewLink(&First, &Second))
				{
					const auto A = Pins.find(static_cast<uintptr_t>(First.Get()));
					const auto B = Pins.find(static_cast<uintptr_t>(Second.Get()));
					bool Accepted = false;
					PulseForge::UUID OutputNode;
					PulseForge::UUID InputNode;
					uint8_t InputSlot = 0;
					if (A != Pins.end() && B != Pins.end() && A->second.Output != B->second.Output)
					{
						const auto& Output = A->second.Output ? A->second : B->second;
						const auto& Input = A->second.Output ? B->second : A->second;
						OutputNode = Output.Node;
						InputNode = Input.Node;
						InputSlot = Input.Slot;
						if (const auto LinkID = PulseForge::UUID::Generate(); LinkID)
						{
							auto Candidate = *Draft.Graph;
							Candidate.Connections.push_back({ *LinkID, OutputNode, InputNode, InputSlot });
							const auto Validation = PulseForge::ValidateMaterialGraphSource(Candidate);
							Accepted = Validation.IsValid() || IsOnlyUnconnectedMaterialInputs(Validation);
							if (Accepted)
							{
								const auto Before = Draft;
								Draft.Graph = std::move(Candidate);
								if (Drafts.BeginEdit(Project, Session.Identity.Asset, Before))
									(void)Documents.MarkEdited(Session.Identity);
							}
						}
					}
					if (Accepted) NodeEditor::AcceptNewItem();
					else NodeEditor::RejectNewItem();
				}
				NodeEditor::EndCreate();
			}
			if (NodeEditor::BeginDelete())
			{
				NodeEditor::LinkId Deleted;
				while (NodeEditor::QueryDeletedLink(&Deleted))
				{
					const auto Link = Connections.find(static_cast<uintptr_t>(Deleted.Get()));
					if (Link == Connections.end())
					{
						NodeEditor::RejectDeletedItem();
						continue;
					}
					NodeEditor::AcceptDeletedItem();
					const auto Before = Draft;
					std::erase_if(Draft.Graph->Connections, [&](const auto& Connection) { return Connection.ID == Link->second; });
					if (Drafts.BeginEdit(Project, Session.Identity.Asset, Before))
						(void)Documents.MarkEdited(Session.Identity);
				}
				NodeEditor::EndDelete();
			}
			NodeEditor::End();
			NodeEditor::SetCurrentEditor(Previous == State.Canvas ? nullptr : Previous);

			if (!UpdatedPositions.empty())
			{
				const auto Before = Draft;
				for (const auto& [NodeID, Position] : UpdatedPositions)
					if (auto Node = std::find_if(Draft.Graph->Nodes.begin(), Draft.Graph->Nodes.end(),
						[NodeID](const auto& Candidate) { return Candidate.ID == NodeID; }); Node != Draft.Graph->Nodes.end())
					{
						Node->X = Position.x;
						Node->Y = Position.y;
					}
				if (Drafts.BeginEdit(Project, Session.Identity.Asset, Before))
					(void)Documents.MarkEdited(Session.Identity);
			}
		}
	}

	EditorMaterialDocumentEditor::EditorMaterialDocumentEditor(EditorDocumentManager& Documents,
		EditorDocumentEditorRegistry& Editors, EditorMaterialDraftController& Drafts, EditorMaterialDetailsPanel& Panel,
		EditorAssetSelector& AssetSelector, EditorStyle& Style, EditorMaterialDetailsActionSink& Actions,
		ProjectProvider Project, AssetListProvider Assets)
		: m_Documents(Documents), m_Editors(Editors), m_Drafts(Drafts), m_Panel(Panel), m_AssetSelector(AssetSelector),
		  m_Style(Style), m_Actions(Actions), m_ProjectProvider(std::move(Project)), m_AssetListProvider(std::move(Assets))
	{
	}

	std::expected<void, std::string> EditorMaterialDocumentEditor::Register()
	{
		EditorDocumentEditorAdapter Adapter;
		Adapter.Kind = EditorDocumentKind::Material;
		Adapter.IsEligible = [this](const auto& Context) { return IsEligible(Context); };
		Adapter.LoadSession = [this](const auto& Context) { return LoadSession(Context); };
		Adapter.DrawWorkspace = [this](auto& Session) { DrawWorkspace(Session); };
		Adapter.Save = [this](auto& Session, uint64_t) { return SaveSession(Session); };
		Adapter.Discard = [this](auto& Session) { return DiscardSession(Session); };
		Adapter.FocusedCommand = [this](auto& Session, auto Command) { return DispatchFocusedCommand(Session, Command); };
		Adapter.AssetLocationChanged = [this](auto& Session, const auto& Path, std::string_view Name)
		{ return UpdateAssetLocation(Session, Path, Name); };
		Adapter.PrepareClose = [this](const auto& Context) { return PrepareClose(Context); };
		return m_Editors.Register(std::move(Adapter));
	}

	bool EditorMaterialDocumentEditor::Open(EditorDocumentSession& Session, const EditorDocumentEditorContext& Context) const
	{
		return Session.Identity.Kind == EditorDocumentKind::Material && m_Editors.Load(Session, Context);
	}

	bool EditorMaterialDocumentEditor::IsEligible(const EditorDocumentEditorContext& Context) const
	{
		if (Context.Identity.Kind != EditorDocumentKind::Material || !Context.Registry) return false;
		const auto Asset = Context.Registry->Find(Context.Identity.Asset);
		return Asset && PulseForge::HasAssetTypeExtension(Asset->ProjectRelativePath, PulseForge::AssetFileType::Material);
	}

	std::expected<std::unique_ptr<EditorDocumentEditorState>, std::string>
	EditorMaterialDocumentEditor::LoadSession(const EditorDocumentEditorContext& Context) const
	{
		PulseForge::Project* const Project = m_ProjectProvider ? m_ProjectProvider() : nullptr;
		if (!Project || Project->GetDescription().ID != Context.Identity.Project ||
			&Project->GetAssetRegistry() != Context.Registry)
			return std::unexpected("The active project changed while opening this material document");
		auto Draft = m_Drafts.LoadDraft(*Project, Context.Identity.Asset);
		if (!Draft) return std::unexpected("Could not open material draft: " + Draft.error());
		return std::unique_ptr<EditorDocumentEditorState>(std::make_unique<State>());
	}

	void EditorMaterialDocumentEditor::DrawWorkspace(EditorDocumentSession& Session)
	{
		State* const Current = dynamic_cast<State*>(Session.EditorState.get());
		PulseForge::Project* const Project = m_ProjectProvider ? m_ProjectProvider() : nullptr;
		if (!Current || !Project || Project->GetDescription().ID != Session.Identity.Project)
		{
			m_Style.EmptyState(EditorIcon::Warning, "The material document project is no longer active.");
			return;
		}
		if (ImGui::Button("Undo")) (void)DispatchFocusedCommand(Session, EditorDocumentCommand::Undo);
		ImGui::SameLine();
		if (ImGui::Button("Redo")) (void)DispatchFocusedCommand(Session, EditorDocumentCommand::Redo);
		ImGui::SameLine();
		ImGui::BeginDisabled(!Session.IsDirty());
		if (ImGui::Button("Save"))
		{
			if (!SaveSession(Session) && Current->Error.empty()) Current->Error = "The material could not be saved.";
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!Session.IsDirty());
		if (ImGui::Button("Discard") && !DiscardSession(Session) && Current->Error.empty())
			Current->Error = "The material draft could not be reloaded.";
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (Session.IsDirty()) ImGui::TextDisabled("Unsaved changes");
		else ImGui::TextDisabled("Saved");
		ImGui::Separator();
		const std::vector<PulseForge::AssetRecord> Assets = m_AssetListProvider();
		m_Panel.DrawEditorContents(*Project, Session.Identity.Asset, Assets, m_Drafts,
			m_AssetSelector, m_Style, m_Actions, false);
		if (PulseForge::MaterialAssetDesc* Draft = m_Drafts.FindDraft(*Project, Session.Identity.Asset))
		{
			ImGui::SeparatorText("Material Graph Source");
			if (!Draft->Graph)
			{
				if (ImGui::Button("Convert fixed material to graph"))
				{
					const PulseForge::MaterialAssetDesc Before = *Draft;
					auto Graph = EditorMaterialDraftController::ConvertFixedMaterialToGraph(*Draft);
					if (!Graph)
						Current->Error = Graph.error();
					else
					{
						if (m_Drafts.BeginEdit(*Project, Session.Identity.Asset, Before))
							(void)m_Documents.MarkEdited(Session.Identity);
						Current->Error.clear();
					}
				}
				ImGui::TextWrapped("Converts the fixed base-color factor to an RGB graph node. Existing texture, alpha, surface, and sidedness settings stay on this Material asset.");
			}
			else
			{
				const auto Validation = PulseForge::ValidateMaterialGraphSource(*Draft->Graph);
				if (!Validation.IsValid())
					ImGui::TextColored(m_Style.GetColor(EditorColorToken::Error), "Graph source is invalid: %s",
						Validation.Diagnostics.front().Message.c_str());
				else
					ImGui::TextDisabled("Graph source is valid. Cooking compiles this graph to a Vulkan SPIR-V product.");
				DrawMaterialGraphCanvas(*Current, Session, *Project, *Draft, m_Drafts, m_Documents);
				ImGui::TextDisabled("Scalar graph outputs replicate to RGB. RGB graph values feed PBR base color; other material and texture settings remain on this Material asset.");
			}
		}
		ImGui::SeparatorText("Draft Preview");
		if (m_Preview)
		{
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			const float Width = (std::min)(Available.x, 480.0f);
			const float Height = Width * (240.0f / 320.0f);
			const uint64_t DraftContentRevision = m_Drafts.GetContentRevision(*Project, Session.Identity.Asset);
			const bool Shown = m_Preview->Draw(Session.SessionGeneration, DraftContentRevision, Width, Height);
			const std::string& PreviewError = m_Preview->GetError(Session.SessionGeneration);
			if (!PreviewError.empty())
			{
				ImGui::TextWrapped(Shown ? "Last complete preview retained. New draft preview unavailable: %s" :
					"Preview unavailable: %s", PreviewError.c_str());
				if (ImGui::SmallButton("Retry Preview"))
					(void)m_Preview->RequestRetry(Session.SessionGeneration, DraftContentRevision);
			}
			else if (!Shown) ImGui::TextDisabled("Preparing the fixed-PBR preview…");
		}
		else ImGui::TextDisabled("The material preview renderer is not available.");
		if (!Current->Error.empty()) ImGui::TextWrapped("%s", Current->Error.c_str());
	}

	bool EditorMaterialDocumentEditor::SaveSession(EditorDocumentSession& Session)
	{
		State* const Current = dynamic_cast<State*>(Session.EditorState.get());
		if (!Current || !m_Actions.SaveMaterialFromDetails(Session.Identity.Asset))
		{
			if (Current) Current->Error = "Save failed. The draft and history remain available for retry.";
			return false;
		}
		Current->Error.clear();
		return true;
	}

	bool EditorMaterialDocumentEditor::DiscardSession(EditorDocumentSession& Session)
	{
		State* const Current = dynamic_cast<State*>(Session.EditorState.get());
		if (!Current || !m_Actions.RevertMaterialFromDetails(Session.Identity.Asset))
		{
			if (Current) Current->Error = "Reload failed. The current draft remains available.";
			return false;
		}
		Current->Error.clear();
		return true;
	}

	bool EditorMaterialDocumentEditor::DispatchFocusedCommand(EditorDocumentSession& Session, EditorDocumentCommand Command)
	{
		PulseForge::Project* const Project = m_ProjectProvider ? m_ProjectProvider() : nullptr;
		if (!Project || Project->GetDescription().ID != Session.Identity.Project) return false;
		return m_Drafts.ApplyHistoryStep(*Project, Session.Identity.Asset, Command == EditorDocumentCommand::Redo, m_Documents)
			.value_or(false);
	}

	bool EditorMaterialDocumentEditor::UpdateAssetLocation(EditorDocumentSession& Session,
		const std::filesystem::path& Path, std::string_view)
	{
		return Session.Identity.Kind == EditorDocumentKind::Material && !Path.empty();
	}

	std::expected<void, std::string> EditorMaterialDocumentEditor::PrepareClose(const EditorDocumentCloseContext& Context) const
	{
		if (Context.Identity.Kind != EditorDocumentKind::Material || !Context.EditorState)
			return std::unexpected("The material document session is unavailable");
		return {};
	}
}
