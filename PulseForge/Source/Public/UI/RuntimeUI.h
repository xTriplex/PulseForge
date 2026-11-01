#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <expected>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	enum class RuntimeUIElementKind : uint8_t
	{
		Image,
		Text,
		Button
	};

	struct RuntimeUIElement final
	{
		UUID ID;
		UUID Parent;
		RuntimeUIElementKind Kind = RuntimeUIElementKind::Image;
		int32_t Order = 0;
		glm::vec2 AnchorMinimum{ 0.0f };
		glm::vec2 AnchorMaximum{ 0.0f };
		glm::vec2 OffsetMinimum{ 0.0f };
		glm::vec2 OffsetMaximum{ 100.0f };
		glm::vec2 Pivot{ 0.0f };
		glm::vec4 Color{ 1.0f };
		glm::vec4 TextColor{ 1.0f };
		AssetID Image;
		AssetID Font;
		float FontSize = 24.0f;
		std::string Text;
		std::string Action;
		bool Visible = true;
		bool Enabled = true;
		bool HitTestVisible = true;
		bool ClipChildren = false;
	};

	struct RuntimeUIDocument final
	{
		static constexpr uint32_t CurrentVersion = 1;
		static constexpr size_t MaximumElements = 2048;
		static constexpr size_t MaximumHierarchyDepth = 32;
		static constexpr size_t MaximumTextBytes = 16384;

		uint32_t Version = CurrentVersion;
		glm::vec2 DesignSize{ 960.0f, 540.0f };
		std::vector<RuntimeUIElement> Elements;
	};

	struct RuntimeUIError final
	{
		std::string Message;
	};

	struct RuntimeUIRect final
	{
		float Left = 0.0f;
		float Top = 0.0f;
		float Right = 0.0f;
		float Bottom = 0.0f;

		[[nodiscard]] bool IsEmpty() const noexcept { return !(Right > Left && Bottom > Top); }
		[[nodiscard]] bool Contains(glm::vec2 Point) const noexcept
		{
			return Point.x >= Left && Point.y >= Top && Point.x < Right && Point.y < Bottom;
		}
	};

	struct RuntimeUILayoutEntry final
	{
		UUID Element;
		RuntimeUIRect Bounds;
		RuntimeUIRect Clip;
		uint32_t PaintOrder = 0;
		bool Visible = false;
		bool Enabled = false;
	};

	struct RuntimeUIViewport final
	{
		// Logical UI dimensions and framebuffer dimensions are supplied independently by the owning viewport.
		glm::vec2 LogicalSize{ 0.0f };
		uint32_t FramebufferWidth = 0;
		uint32_t FramebufferHeight = 0;
	};

	enum class RuntimeUIInputKind : uint8_t
	{
		PointerMoved,
		PointerPressed,
		PointerReleased,
		Navigate,
		Activate,
		Cancel,
		FocusLost,
		CaptureCancelled
	};

	struct RuntimeUIInputEvent final
	{
		RuntimeUIInputKind Kind = RuntimeUIInputKind::PointerMoved;
		glm::vec2 Position{ 0.0f };
		glm::vec2 Direction{ 0.0f };
	};

	struct RuntimeUIAction final
	{
		UUID Element;
		std::string Name;
		uint64_t TreeGeneration = 0;
	};

	struct RuntimeUIDrawCommand final
	{
		UUID Element;
		RuntimeUIElementKind Kind = RuntimeUIElementKind::Image;
		RuntimeUIRect Bounds;
		RuntimeUIRect Clip;
		glm::vec4 Color{ 1.0f };
		glm::vec4 TextColor{ 1.0f };
		AssetID Image;
		AssetID Font;
		float FontSize = 24.0f;
		std::string Text;
		bool Focused = false;
		bool Hovered = false;
	};

	class PULSEFORGE_API RuntimeUIController final
	{
	public:
		[[nodiscard]] std::expected<std::vector<RuntimeUILayoutEntry>, RuntimeUIError> Layout(
			const RuntimeUIDocument& Document,
			const RuntimeUIViewport& Viewport,
			uint64_t TreeGeneration);
		[[nodiscard]] std::vector<RuntimeUIAction> ProcessInput(
			const RuntimeUIDocument& Document,
			std::span<const RuntimeUILayoutEntry> Layout,
			std::span<const RuntimeUIInputEvent> Events,
			uint64_t TreeGeneration);
		[[nodiscard]] std::optional<UUID> GetFocusedElement() const noexcept { return m_Focused; }
		[[nodiscard]] std::optional<UUID> GetHoveredElement() const noexcept { return m_Hovered; }
		void Reset(uint64_t TreeGeneration) noexcept;

	private:
		uint64_t m_TreeGeneration = 0;
		std::optional<UUID> m_Focused;
		std::optional<UUID> m_Hovered;
		std::optional<UUID> m_Captured;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<void, RuntimeUIError> ValidateRuntimeUIDocument(const RuntimeUIDocument& Document);
	[[nodiscard]] PULSEFORGE_API std::expected<RuntimeUIDocument, RuntimeUIError> ParseRuntimeUIDocument(std::string_view Json);
	[[nodiscard]] PULSEFORGE_API std::expected<std::string, RuntimeUIError> SerializeRuntimeUIDocument(const RuntimeUIDocument& Document);
	[[nodiscard]] PULSEFORGE_API std::expected<std::vector<RuntimeUILayoutEntry>, RuntimeUIError> ComputeRuntimeUILayout(
		const RuntimeUIDocument& Document,
		const RuntimeUIViewport& Viewport);
	[[nodiscard]] PULSEFORGE_API std::optional<UUID> HitTestRuntimeUI(
		const RuntimeUIDocument& Document,
		std::span<const RuntimeUILayoutEntry> Layout,
		glm::vec2 Position) noexcept;
	[[nodiscard]] PULSEFORGE_API std::vector<RuntimeUIDrawCommand> BuildRuntimeUIDrawList(
		const RuntimeUIDocument& Document,
		std::span<const RuntimeUILayoutEntry> Layout,
		std::optional<UUID> Focused,
		std::optional<UUID> Hovered);
} // namespace PulseForge
