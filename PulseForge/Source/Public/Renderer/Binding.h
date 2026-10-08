#pragma once

#include "Core/Core.h"
#include "Renderer/Buffer.h"
#include "Renderer/Graphics.h"
#include "Renderer/Texture.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace PulseForge
{
	enum class BindingResourceType : uint8_t
	{
		Texture2D,
		TextureCube,
		Sampler,
		ConstantBuffer
	};

	enum class ShaderVisibility : uint8_t
	{
		Vertex,
		Fragment,
		AllGraphics
	};

	struct BindingLayoutItemDesc
	{
		BindingResourceType Type = BindingResourceType::ConstantBuffer;
		uint32_t Slot = 0;
	};

	struct BindingLayoutDesc
	{
		ShaderVisibility Visibility = ShaderVisibility::Fragment;
		std::vector<BindingLayoutItemDesc> Items;
		std::string DebugName;
	};

	class PULSEFORGE_API BindingLayout
	{
	public:
		virtual ~BindingLayout() = default;
		[[nodiscard]] virtual const BindingLayoutDesc& GetDescription() const noexcept = 0;
	};

	struct BufferBindingDesc
	{
		uint32_t Slot = 0;
		// The buffer is borrowed only while CreateBindingSet runs. The created set retains
		// the backend resource independently of this wrapper's lifetime.
		std::reference_wrapper<const Buffer> Resource;
	};

	struct TextureBindingDesc
	{
		uint32_t Slot = 0;
		// The texture is borrowed while creating the set; the backend set retains its GPU resource.
		std::reference_wrapper<const Texture> Resource;
		BindingResourceType Type = BindingResourceType::Texture2D;
	};

	struct SamplerBindingDesc
	{
		uint32_t Slot = 0;
		// The sampler is borrowed while creating the set; the backend set retains its GPU resource.
		std::reference_wrapper<const Sampler> Resource;
	};

	struct BindingSetDesc
	{
		BindingLayoutHandle Layout;
		std::vector<TextureBindingDesc> Textures;
		std::vector<SamplerBindingDesc> Samplers;
		std::vector<BufferBindingDesc> Buffers;
	};

	class PULSEFORGE_API BindingSet
	{
	public:
		virtual ~BindingSet() = default;
		[[nodiscard]] virtual const BindingLayout& GetLayout() const noexcept = 0;
	};

	using BindingSetHandle = std::unique_ptr<BindingSet>;

	enum class BindingErrorCode : uint8_t
	{
		InvalidLayout,
		InvalidSet,
		UnsupportedFeature,
		BackendFailure
	};

	struct BindingError
	{
		BindingErrorCode Code;
		std::string Message;
	};

	using BindingLayoutCreateResult = std::expected<BindingLayoutHandle, BindingError>;
	using BindingSetCreateResult = std::expected<BindingSetHandle, BindingError>;

	[[nodiscard]] PULSEFORGE_API std::expected<void, BindingError> ValidateBindingLayout(
		const BindingLayoutDesc& Description);
	[[nodiscard]] PULSEFORGE_API std::expected<void, BindingError> ValidateBindingSet(
		const BindingSetDesc& Description);
}
