#include "UI/RuntimeUI.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace PulseForge
{
	namespace
	{
		using Json = nlohmann::json;

		RuntimeUIError MakeError(std::string Message)
		{
			return { std::move(Message) };
		}

		bool IsFinite(glm::vec2 Value) noexcept
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y);
		}

		bool IsFinite(glm::vec4 Value) noexcept
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) && std::isfinite(Value.w);
		}

		std::expected<UUID, RuntimeUIError> ParseIdentifier(const Json& Value, bool AllowNil)
		{
			if (Value.is_null() && AllowNil) return UUID{};
			if (!Value.is_string()) return std::unexpected(MakeError("UI element identity must be a UUID string"));
			auto Parsed = UUID::Parse(Value.get<std::string>());
			if (!Parsed || (Parsed->IsNil() && !AllowNil))
				return std::unexpected(MakeError("UI element identity must be a valid non-nil UUID"));
			return *Parsed;
		}

		std::expected<AssetID, RuntimeUIError> ParseAsset(const Json& Value)
		{
			if (Value.is_null()) return AssetID{};
			if (!Value.is_string()) return std::unexpected(MakeError("UI resource reference must be a UUID string or null"));
			auto Parsed = UUID::Parse(Value.get<std::string>());
			if (!Parsed || Parsed->IsNil()) return std::unexpected(MakeError("UI resource reference must be a non-nil UUID"));
			return *Parsed;
		}

		template<size_t Count>
		bool ReadVector(const Json& Value, std::array<float, Count>& Components)
		{
			if (!Value.is_array() || Value.size() != Count) return false;
			for (size_t Index = 0; Index < Count; ++Index)
			{
				if (!Value[Index].is_number()) return false;
				Components[Index] = Value[Index].get<float>();
				if (!std::isfinite(Components[Index])) return false;
			}
			return true;
		}

		std::expected<glm::vec2, RuntimeUIError> ParseVec2(const Json& Value, std::string_view Label)
		{
			std::array<float, 2> Components{};
			if (!ReadVector(Value, Components)) return std::unexpected(MakeError(std::string(Label) + " must contain two finite numbers"));
			return glm::vec2{ Components[0], Components[1] };
		}

		std::expected<glm::vec4, RuntimeUIError> ParseVec4(const Json& Value, std::string_view Label)
		{
			std::array<float, 4> Components{};
			if (!ReadVector(Value, Components)) return std::unexpected(MakeError(std::string(Label) + " must contain four finite numbers"));
			return glm::vec4{ Components[0], Components[1], Components[2], Components[3] };
		}

		Json ToJson(glm::vec2 Value)
		{
			return Json::array({ Value.x, Value.y });
		}

		Json ToJson(glm::vec4 Value)
		{
			return Json::array({ Value.r, Value.g, Value.b, Value.a });
		}

		std::optional<RuntimeUIElementKind> ParseKind(std::string_view Value) noexcept
		{
			if (Value == "image") return RuntimeUIElementKind::Image;
			if (Value == "text") return RuntimeUIElementKind::Text;
			if (Value == "button") return RuntimeUIElementKind::Button;
			return std::nullopt;
		}

		std::string_view KindName(RuntimeUIElementKind Value) noexcept
		{
			switch (Value)
			{
			case RuntimeUIElementKind::Image: return "image";
			case RuntimeUIElementKind::Text: return "text";
			case RuntimeUIElementKind::Button: return "button";
			}
			return "invalid";
		}

		std::optional<RuntimeUIRect> Intersect(RuntimeUIRect A, RuntimeUIRect B) noexcept
		{
			RuntimeUIRect Result{ (std::max)(A.Left, B.Left), (std::max)(A.Top, B.Top),
				(std::min)(A.Right, B.Right), (std::min)(A.Bottom, B.Bottom) };
			if (Result.IsEmpty()) return std::nullopt;
			return Result;
		}

		bool IsValidUtf8(std::string_view Text) noexcept
		{
			for (size_t Index = 0; Index < Text.size();)
			{
				const uint8_t First = static_cast<uint8_t>(Text[Index]);
				if (First <= 0x7f) { ++Index; continue; }
				uint32_t Codepoint = 0;
				size_t Continuations = 0;
				if ((First & 0xe0u) == 0xc0u) { Codepoint = First & 0x1fu; Continuations = 1; if (Codepoint < 2) return false; }
				else if ((First & 0xf0u) == 0xe0u) { Codepoint = First & 0x0fu; Continuations = 2; }
				else if ((First & 0xf8u) == 0xf0u) { Codepoint = First & 0x07u; Continuations = 3; }
				else return false;
				if (Continuations > Text.size() - Index - 1) return false;
				for (size_t Offset = 1; Offset <= Continuations; ++Offset)
				{
					const uint8_t Next = static_cast<uint8_t>(Text[Index + Offset]);
					if ((Next & 0xc0u) != 0x80u) return false;
					Codepoint = (Codepoint << 6) | (Next & 0x3fu);
				}
				if ((Continuations == 2 && Codepoint < 0x800) || (Continuations == 3 && Codepoint < 0x10000) ||
					Codepoint > 0x10ffff || (Codepoint >= 0xd800 && Codepoint <= 0xdfff)) return false;
				Index += Continuations + 1;
			}
			return true;
		}

	} // namespace

	std::expected<void, RuntimeUIError> ValidateRuntimeUIDocument(const RuntimeUIDocument& Document)
	{
		if (Document.Version != RuntimeUIDocument::CurrentVersion)
			return std::unexpected(MakeError("UI document version is unsupported"));
		if (!IsFinite(Document.DesignSize) || Document.DesignSize.x <= 0.0f || Document.DesignSize.y <= 0.0f ||
			Document.DesignSize.x > 16384.0f || Document.DesignSize.y > 16384.0f)
			return std::unexpected(MakeError("UI design size must be finite, positive, and no larger than 16384 units"));
		if (Document.Elements.size() > RuntimeUIDocument::MaximumElements)
			return std::unexpected(MakeError("UI document exceeds the 2048 element limit"));

		std::unordered_map<UUID, size_t, UUIDHash> Indices;
		Indices.reserve(Document.Elements.size());
		for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
		{
			const RuntimeUIElement& Element = Document.Elements[Index];
			if (Element.ID.IsNil() || !Indices.emplace(Element.ID, Index).second)
				return std::unexpected(MakeError("UI element IDs must be non-nil and unique within the document"));
			if (!IsFinite(Element.AnchorMinimum) || !IsFinite(Element.AnchorMaximum) ||
				Element.AnchorMinimum.x < 0.0f || Element.AnchorMinimum.y < 0.0f ||
				Element.AnchorMaximum.x > 1.0f || Element.AnchorMaximum.y > 1.0f ||
				Element.AnchorMinimum.x > Element.AnchorMaximum.x || Element.AnchorMinimum.y > Element.AnchorMaximum.y)
				return std::unexpected(MakeError("UI anchors must be finite, ordered, and within [0,1]"));
			if (!IsFinite(Element.OffsetMinimum) || !IsFinite(Element.OffsetMaximum) || !IsFinite(Element.Pivot) ||
				Element.Pivot.x < 0.0f || Element.Pivot.x > 1.0f || Element.Pivot.y < 0.0f || Element.Pivot.y > 1.0f ||
				!IsFinite(Element.Color) || !IsFinite(Element.TextColor) || Element.Color.r < 0.0f || Element.Color.g < 0.0f || Element.Color.b < 0.0f ||
				Element.Color.a < 0.0f || Element.Color.r > 1.0f || Element.Color.g > 1.0f || Element.Color.b > 1.0f ||
				Element.Color.a > 1.0f || Element.TextColor.r < 0.0f || Element.TextColor.g < 0.0f || Element.TextColor.b < 0.0f ||
				Element.TextColor.a < 0.0f || Element.TextColor.r > 1.0f || Element.TextColor.g > 1.0f ||
				Element.TextColor.b > 1.0f || Element.TextColor.a > 1.0f)
				return std::unexpected(MakeError("UI offsets, pivot, and color contain invalid values"));
			if (Element.Kind != RuntimeUIElementKind::Image && Element.Kind != RuntimeUIElementKind::Text &&
				Element.Kind != RuntimeUIElementKind::Button)
				return std::unexpected(MakeError("UI element kind is unsupported"));
			if (Element.Text.size() > RuntimeUIDocument::MaximumTextBytes || !IsValidUtf8(Element.Text))
				return std::unexpected(MakeError("UI text must be well-formed UTF-8 and no longer than 16 KiB"));
			if (Element.Action.size() > 128 || Element.Action.find_first_of("\r\n") != std::string::npos ||
				Element.Action.find('\0') != std::string::npos)
				return std::unexpected(MakeError("UI button action must be a single line of at most 128 bytes"));
			if (!std::isfinite(Element.FontSize) || Element.FontSize <= 0.0f || Element.FontSize > 256.0f)
				return std::unexpected(MakeError("UI font size must be finite and between 0 and 256 logical units"));
			if (Element.Kind == RuntimeUIElementKind::Image && Element.Image.IsNil())
				return std::unexpected(MakeError("UI image element requires a managed image asset identity"));
			if (Element.Kind == RuntimeUIElementKind::Text && Element.Font.IsNil())
				return std::unexpected(MakeError("UI text element requires a managed font asset identity"));
			if (Element.Kind == RuntimeUIElementKind::Button && !Element.Text.empty() && Element.Font.IsNil())
				return std::unexpected(MakeError("UI button text requires a managed font asset identity"));
			if (Element.Kind == RuntimeUIElementKind::Button && Element.Action.empty())
				return std::unexpected(MakeError("UI button requires a non-empty action name"));
		}

		std::vector<uint8_t> Marks(Document.Elements.size());
		std::function<std::expected<size_t, RuntimeUIError>(size_t)> Visit = [&](size_t Index) -> std::expected<size_t, RuntimeUIError>
		{
			if (Marks[Index] == 1) return std::unexpected(MakeError("UI hierarchy contains a parent cycle"));
			if (Marks[Index] == 2) return size_t{ 1 };
			Marks[Index] = 1;
			const UUID Parent = Document.Elements[Index].Parent;
			size_t Depth = 1;
			if (!Parent.IsNil())
			{
				const auto ParentIt = Indices.find(Parent);
				if (ParentIt == Indices.end()) return std::unexpected(MakeError("UI element references a missing parent"));
				auto ParentDepth = Visit(ParentIt->second);
				if (!ParentDepth) return std::unexpected(ParentDepth.error());
				Depth = *ParentDepth + 1;
			}
			if (Depth > RuntimeUIDocument::MaximumHierarchyDepth)
				return std::unexpected(MakeError("UI hierarchy exceeds the 32 level depth limit"));
			Marks[Index] = 2;
			return Depth;
		};
		for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
			if (auto Depth = Visit(Index); !Depth) return std::unexpected(Depth.error());
		return {};
	}

	std::expected<RuntimeUIDocument, RuntimeUIError> ParseRuntimeUIDocument(std::string_view Text)
	{
		if (Text.empty() || Text.size() > 2u * 1024u * 1024u)
			return std::unexpected(MakeError("UI document must contain between 1 byte and 2 MiB"));
		try
		{
			const Json Root = Json::parse(Text.begin(), Text.end());
			if (!Root.is_object() || !Root.contains("version") || !Root.contains("designSize") || !Root.contains("elements") ||
				!Root["version"].is_number_integer() || !Root["elements"].is_array())
				return std::unexpected(MakeError("UI document root must contain version, designSize, and elements"));
			RuntimeUIDocument Document;
			Document.Version = Root["version"].get<uint32_t>();
			auto DesignSize = ParseVec2(Root["designSize"], "UI designSize");
			if (!DesignSize) return std::unexpected(DesignSize.error());
			Document.DesignSize = *DesignSize;
			if (Root["elements"].size() > RuntimeUIDocument::MaximumElements)
				return std::unexpected(MakeError("UI document exceeds the 2048 element limit"));
			for (const Json& Value : Root["elements"])
			{
				if (!Value.is_object()) return std::unexpected(MakeError("UI elements must be JSON objects"));
				RuntimeUIElement Element;
				auto ID = ParseIdentifier(Value.value("id", Json{}), false);
				auto Parent = ParseIdentifier(Value.value("parent", Json{}), true);
				if (!ID || !Parent) return std::unexpected(!ID ? ID.error() : Parent.error());
				Element.ID = *ID;
				Element.Parent = *Parent;
				if (!Value.contains("kind") || !Value["kind"].is_string())
					return std::unexpected(MakeError("UI element kind is required"));
				auto Kind = ParseKind(Value["kind"].get<std::string>());
				if (!Kind) return std::unexpected(MakeError("UI element kind is unsupported"));
				Element.Kind = *Kind;
				Element.Order = Value.value("order", 0);
				auto AnchorMin = ParseVec2(Value.value("anchorMinimum", Json::array({ 0.0f, 0.0f })), "anchorMinimum");
				auto AnchorMax = ParseVec2(Value.value("anchorMaximum", Json::array({ 0.0f, 0.0f })), "anchorMaximum");
				auto OffsetMin = ParseVec2(Value.value("offsetMinimum", Json::array({ 0.0f, 0.0f })), "offsetMinimum");
				auto OffsetMax = ParseVec2(Value.value("offsetMaximum", Json::array({ 100.0f, 100.0f })), "offsetMaximum");
				auto Pivot = ParseVec2(Value.value("pivot", Json::array({ 0.0f, 0.0f })), "pivot");
				auto Color = ParseVec4(Value.value("color", Json::array({ 1.0f, 1.0f, 1.0f, 1.0f })), "color");
				auto TextColor = ParseVec4(Value.value("textColor", Json::array({ 1.0f, 1.0f, 1.0f, 1.0f })), "textColor");
				if (!AnchorMin || !AnchorMax || !OffsetMin || !OffsetMax || !Pivot || !Color || !TextColor)
					return std::unexpected(!AnchorMin ? AnchorMin.error() : !AnchorMax ? AnchorMax.error() : !OffsetMin ? OffsetMin.error() :
						!OffsetMax ? OffsetMax.error() : !Pivot ? Pivot.error() : !Color ? Color.error() : TextColor.error());
				Element.AnchorMinimum = *AnchorMin;
				Element.AnchorMaximum = *AnchorMax;
				Element.OffsetMinimum = *OffsetMin;
				Element.OffsetMaximum = *OffsetMax;
				Element.Pivot = *Pivot;
				Element.Color = *Color;
				Element.TextColor = *TextColor;
				auto Image = ParseAsset(Value.value("image", Json{}));
				auto Font = ParseAsset(Value.value("font", Json{}));
				if (!Image || !Font) return std::unexpected(!Image ? Image.error() : Font.error());
				Element.Image = *Image;
				Element.Font = *Font;
				Element.FontSize = Value.value("fontSize", 24.0f);
				Element.Text = Value.value("text", std::string{});
				Element.Action = Value.value("action", std::string{});
				Element.Visible = Value.value("visible", true);
				Element.Enabled = Value.value("enabled", true);
				Element.HitTestVisible = Value.value("hitTestVisible", true);
				Element.ClipChildren = Value.value("clipChildren", false);
				Document.Elements.push_back(std::move(Element));
			}
			if (auto Valid = ValidateRuntimeUIDocument(Document); !Valid) return std::unexpected(Valid.error());
			return Document;
		}
		catch (const Json::exception& Exception)
		{
			return std::unexpected(MakeError(std::string("Could not parse UI document: ") + Exception.what()));
		}
	}

	std::expected<std::string, RuntimeUIError> SerializeRuntimeUIDocument(const RuntimeUIDocument& Document)
	{
		if (auto Valid = ValidateRuntimeUIDocument(Document); !Valid) return std::unexpected(Valid.error());
		Json Root;
		Root["version"] = Document.Version;
		Root["designSize"] = ToJson(Document.DesignSize);
		Root["elements"] = Json::array();
		for (const RuntimeUIElement& Element : Document.Elements)
		{
			Json Value;
			Value["id"] = Element.ID.ToString();
			Value["parent"] = Element.Parent.IsNil() ? Json{} : Json(Element.Parent.ToString());
			Value["kind"] = KindName(Element.Kind);
			Value["order"] = Element.Order;
			Value["anchorMinimum"] = ToJson(Element.AnchorMinimum);
			Value["anchorMaximum"] = ToJson(Element.AnchorMaximum);
			Value["offsetMinimum"] = ToJson(Element.OffsetMinimum);
			Value["offsetMaximum"] = ToJson(Element.OffsetMaximum);
			Value["pivot"] = ToJson(Element.Pivot);
			Value["color"] = ToJson(Element.Color);
			Value["textColor"] = ToJson(Element.TextColor);
			Value["image"] = Element.Image.IsNil() ? Json{} : Json(Element.Image.ToString());
			Value["font"] = Element.Font.IsNil() ? Json{} : Json(Element.Font.ToString());
			Value["fontSize"] = Element.FontSize;
			Value["text"] = Element.Text;
			Value["action"] = Element.Action;
			Value["visible"] = Element.Visible;
			Value["enabled"] = Element.Enabled;
			Value["hitTestVisible"] = Element.HitTestVisible;
			Value["clipChildren"] = Element.ClipChildren;
			Root["elements"].push_back(std::move(Value));
		}
		return Root.dump(2);
	}

	std::expected<std::vector<RuntimeUILayoutEntry>, RuntimeUIError> ComputeRuntimeUILayout(
		const RuntimeUIDocument& Document, const RuntimeUIViewport& Viewport)
	{
		if (auto Valid = ValidateRuntimeUIDocument(Document); !Valid) return std::unexpected(Valid.error());
		if (!IsFinite(Viewport.LogicalSize) || Viewport.LogicalSize.x < 0.0f || Viewport.LogicalSize.y < 0.0f)
			return std::unexpected(MakeError("UI viewport logical dimensions must be finite and non-negative"));
		std::vector<RuntimeUILayoutEntry> Result(Document.Elements.size());
		if (Viewport.FramebufferWidth == 0 || Viewport.FramebufferHeight == 0 ||
			Viewport.LogicalSize.x == 0.0f || Viewport.LogicalSize.y == 0.0f) return Result;

		std::unordered_map<UUID, size_t, UUIDHash> Indices;
		Indices.reserve(Document.Elements.size());
		for (size_t Index = 0; Index < Document.Elements.size(); ++Index) Indices.emplace(Document.Elements[Index].ID, Index);
		std::vector<std::vector<size_t>> Children(Document.Elements.size());
		std::vector<size_t> Roots;
		for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
		{
			const UUID Parent = Document.Elements[Index].Parent;
			if (Parent.IsNil()) Roots.push_back(Index);
			else Children[Indices.at(Parent)].push_back(Index);
		}
		const auto SortChildren = [&Document](std::vector<size_t>& List)
		{
			std::stable_sort(List.begin(), List.end(), [&](size_t A, size_t B)
			{
				return Document.Elements[A].Order < Document.Elements[B].Order;
			});
		};
		SortChildren(Roots);
		for (auto& List : Children) SortChildren(List);

		const RuntimeUIRect ViewRect{ 0.0f, 0.0f, Viewport.LogicalSize.x, Viewport.LogicalSize.y };
		const glm::vec2 OffsetScale = Viewport.LogicalSize / Document.DesignSize;
		uint32_t PaintOrder = 0;
		std::function<void(size_t, RuntimeUIRect, RuntimeUIRect, bool, bool, size_t)> Visit =
			[&](size_t Index, RuntimeUIRect Parent, RuntimeUIRect ParentClip, bool ParentVisible, bool ParentEnabled, size_t Depth)
			{
				if (Depth > RuntimeUIDocument::MaximumHierarchyDepth) return;
				const RuntimeUIElement& Element = Document.Elements[Index];
				const glm::vec2 Minimum = glm::vec2(Parent.Left, Parent.Top) +
					Element.AnchorMinimum * glm::vec2(Parent.Right - Parent.Left, Parent.Bottom - Parent.Top) + Element.OffsetMinimum * OffsetScale;
				const glm::vec2 Maximum = glm::vec2(Parent.Left, Parent.Top) +
					Element.AnchorMaximum * glm::vec2(Parent.Right - Parent.Left, Parent.Bottom - Parent.Top) + Element.OffsetMaximum * OffsetScale;
				const glm::vec2 Size = Maximum - Minimum;
				const glm::vec2 PivotShift = Size * Element.Pivot;
				RuntimeUIRect Bounds{ Minimum.x - PivotShift.x, Minimum.y - PivotShift.y,
					Minimum.x - PivotShift.x + Size.x, Minimum.y - PivotShift.y + Size.y };
				if (!std::isfinite(Bounds.Left) || !std::isfinite(Bounds.Top) || !std::isfinite(Bounds.Right) || !std::isfinite(Bounds.Bottom))
					Bounds = {};
				const bool Visible = ParentVisible && Element.Visible;
				const bool Enabled = ParentEnabled && Element.Enabled;
				const auto ClipIntersection = Intersect(ParentClip, ViewRect);
				RuntimeUILayoutEntry& Entry = Result[Index];
				Entry.Element = Element.ID;
				Entry.Bounds = Bounds;
				Entry.Clip = ClipIntersection.value_or(RuntimeUIRect{});
				Entry.Visible = Visible && !Entry.Clip.IsEmpty() && !Bounds.IsEmpty();
				Entry.Enabled = Enabled;
				Entry.PaintOrder = PaintOrder++;
				RuntimeUIRect ChildClip = ParentClip;
				if (Element.ClipChildren)
					ChildClip = Intersect(ParentClip, Bounds).value_or(RuntimeUIRect{});
				for (const size_t Child : Children[Index]) Visit(Child, Bounds, ChildClip, Visible, Enabled, Depth + 1);
			};
		for (const size_t Root : Roots) Visit(Root, ViewRect, ViewRect, true, true, 1);
		return Result;
	}

	std::optional<UUID> HitTestRuntimeUI(const RuntimeUIDocument& Document,
		std::span<const RuntimeUILayoutEntry> Layout, glm::vec2 Position) noexcept
	{
		if (Layout.size() != Document.Elements.size() || !IsFinite(Position)) return std::nullopt;
		std::optional<size_t> Hit;
		for (size_t Index = 0; Index < Layout.size(); ++Index)
		{
			const RuntimeUIElement& Element = Document.Elements[Index];
			const RuntimeUILayoutEntry& Entry = Layout[Index];
			if (Element.Kind == RuntimeUIElementKind::Button && Entry.Visible && Entry.Enabled && Element.HitTestVisible &&
				Entry.Bounds.Contains(Position) && Entry.Clip.Contains(Position) &&
				(!Hit || Entry.PaintOrder > Layout[*Hit].PaintOrder)) Hit = Index;
		}
		return Hit ? std::optional<UUID>(Document.Elements[*Hit].ID) : std::nullopt;
	}

	std::vector<RuntimeUIDrawCommand> BuildRuntimeUIDrawList(const RuntimeUIDocument& Document,
		std::span<const RuntimeUILayoutEntry> Layout, std::optional<UUID> Focused, std::optional<UUID> Hovered)
	{
		std::vector<RuntimeUIDrawCommand> Result;
		if (Layout.size() != Document.Elements.size()) return Result;
		Result.reserve(Document.Elements.size());
		std::vector<size_t> Ordered(Document.Elements.size());
		for (size_t Index = 0; Index < Ordered.size(); ++Index) Ordered[Index] = Index;
		std::stable_sort(Ordered.begin(), Ordered.end(), [&](size_t First, size_t Second)
			{ return Layout[First].PaintOrder < Layout[Second].PaintOrder; });
		for (const size_t Index : Ordered)
		{
			const RuntimeUIElement& Element = Document.Elements[Index];
			const RuntimeUILayoutEntry& Entry = Layout[Index];
			if (!Entry.Visible || Entry.Clip.IsEmpty()) continue;
			Result.push_back({ Element.ID, Element.Kind, Entry.Bounds, Entry.Clip, Element.Color, Element.TextColor, Element.Image, Element.Font,
				Element.FontSize, Element.Text, Focused == Element.ID, Hovered == Element.ID });
		}
		return Result;
	}

	std::expected<std::vector<RuntimeUILayoutEntry>, RuntimeUIError> RuntimeUIController::Layout(
		const RuntimeUIDocument& Document, const RuntimeUIViewport& Viewport, uint64_t TreeGeneration)
	{
		if (TreeGeneration == 0) return std::unexpected(MakeError("UI tree generation must be non-zero"));
		if (m_TreeGeneration != TreeGeneration) Reset(TreeGeneration);
		auto Result = ComputeRuntimeUILayout(Document, Viewport);
		if (!Result) return std::unexpected(Result.error());
		const auto FocusExists = [&]()
		{
			if (!m_Focused) return false;
			for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
				if (Document.Elements[Index].ID == *m_Focused && Document.Elements[Index].Kind == RuntimeUIElementKind::Button &&
					(*Result)[Index].Visible && (*Result)[Index].Enabled) return true;
			return false;
		};
		if (!FocusExists())
		{
			m_Focused.reset();
			for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
				if (Document.Elements[Index].Kind == RuntimeUIElementKind::Button && (*Result)[Index].Visible && (*Result)[Index].Enabled &&
					(!m_Focused || (*Result)[Index].PaintOrder < [&]()
					{
						for (size_t Candidate = 0; Candidate < Document.Elements.size(); ++Candidate)
							if (Document.Elements[Candidate].ID == *m_Focused) return (*Result)[Candidate].PaintOrder;
						return (std::numeric_limits<uint32_t>::max)();
					}())) m_Focused = Document.Elements[Index].ID;
		}
		if (m_Hovered)
		{
			bool Exists = false;
			for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
				if (Document.Elements[Index].ID == *m_Hovered && (*Result)[Index].Visible && (*Result)[Index].Enabled &&
					Document.Elements[Index].HitTestVisible) Exists = true;
			if (!Exists) m_Hovered.reset();
		}
		if (m_Captured)
		{
			bool Exists = false;
			for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
				if (Document.Elements[Index].ID == *m_Captured && Document.Elements[Index].Kind == RuntimeUIElementKind::Button &&
					(*Result)[Index].Visible && (*Result)[Index].Enabled && Document.Elements[Index].HitTestVisible)
					Exists = true;
			if (!Exists) m_Captured.reset();
		}
		return Result;
	}

	std::vector<RuntimeUIAction> RuntimeUIController::ProcessInput(
		const RuntimeUIDocument& Document, std::span<const RuntimeUILayoutEntry> Layout,
		std::span<const RuntimeUIInputEvent> Events, uint64_t TreeGeneration)
	{
		std::vector<RuntimeUIAction> Actions;
		if (TreeGeneration == 0 || Layout.size() != Document.Elements.size()) return Actions;
		if (m_TreeGeneration != TreeGeneration) Reset(TreeGeneration);
		auto Activate = [&](UUID Target)
		{
			for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
				if (Document.Elements[Index].ID == Target && Document.Elements[Index].Kind == RuntimeUIElementKind::Button &&
					Layout[Index].Visible && Layout[Index].Enabled && !Document.Elements[Index].Action.empty())
					Actions.push_back({ Target, Document.Elements[Index].Action, TreeGeneration });
		};
		for (const RuntimeUIInputEvent& Event : Events)
		{
			switch (Event.Kind)
			{
			case RuntimeUIInputKind::PointerMoved:
				m_Hovered = HitTestRuntimeUI(Document, Layout, Event.Position);
				if (m_Hovered) m_Focused = m_Hovered;
				break;
			case RuntimeUIInputKind::PointerPressed:
				m_Captured = HitTestRuntimeUI(Document, Layout, Event.Position);
				if (m_Captured) m_Focused = m_Captured;
				break;
			case RuntimeUIInputKind::PointerReleased:
				if (m_Captured && HitTestRuntimeUI(Document, Layout, Event.Position) == m_Captured) Activate(*m_Captured);
				m_Captured.reset();
				break;
			case RuntimeUIInputKind::Navigate:
			{
				std::optional<size_t> Current;
				for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
					if (m_Focused && Document.Elements[Index].ID == *m_Focused) { Current = Index; break; }
				std::vector<size_t> Buttons;
				for (size_t Index = 0; Index < Document.Elements.size(); ++Index)
					if (Document.Elements[Index].Kind == RuntimeUIElementKind::Button && Layout[Index].Visible && Layout[Index].Enabled)
						Buttons.push_back(Index);
				std::stable_sort(Buttons.begin(), Buttons.end(), [&](size_t A, size_t B)
					{ return Layout[A].PaintOrder < Layout[B].PaintOrder; });
				if (Buttons.empty()) { m_Focused.reset(); break; }
				if (!Current) { m_Focused = Document.Elements[Buttons.front()].ID; break; }
				if (!std::isfinite(Event.Direction.x) || !std::isfinite(Event.Direction.y) ||
					(Event.Direction.x == 0.0f && Event.Direction.y == 0.0f)) break;
				const RuntimeUIRect CurrentBounds = Layout[*Current].Bounds;
				const glm::vec2 CurrentCenter{ (CurrentBounds.Left + CurrentBounds.Right) * 0.5f,
					(CurrentBounds.Top + CurrentBounds.Bottom) * 0.5f };
				const glm::vec2 Direction = glm::normalize(Event.Direction);
				float BestScore = (std::numeric_limits<float>::max)();
				std::optional<size_t> Best;
				for (const size_t Candidate : Buttons)
				{
					if (Candidate == *Current) continue;
					const RuntimeUIRect Bounds = Layout[Candidate].Bounds;
					const glm::vec2 Delta{ (Bounds.Left + Bounds.Right) * 0.5f - CurrentCenter.x,
						(Bounds.Top + Bounds.Bottom) * 0.5f - CurrentCenter.y };
					const float Forward = glm::dot(Delta, Direction);
					if (Forward <= 0.0f) continue;
					const float Lateral = std::abs(Delta.x * Direction.y - Delta.y * Direction.x);
					const float Score = Forward + Lateral * 2.0f;
					if (Score < BestScore) { BestScore = Score; Best = Candidate; }
				}
				if (Best) m_Focused = Document.Elements[*Best].ID;
				break;
			}
			case RuntimeUIInputKind::Activate:
				if (m_Focused) Activate(*m_Focused);
				break;
			case RuntimeUIInputKind::Cancel:
				m_Captured.reset();
				break;
			case RuntimeUIInputKind::FocusLost:
			case RuntimeUIInputKind::CaptureCancelled:
				m_Captured.reset();
				m_Hovered.reset();
				break;
			}
		}
		return Actions;
	}

	void RuntimeUIController::Reset(uint64_t TreeGeneration) noexcept
	{
		m_TreeGeneration = TreeGeneration;
		m_Focused.reset();
		m_Hovered.reset();
		m_Captured.reset();
	}
} // namespace PulseForge
