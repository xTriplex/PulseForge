#include "Core/PulseForgePCH.h"
#include "Renderer/Binding.h"

#include <array>

namespace PulseForge
{
	namespace
	{
		std::expected<void, BindingError> MakeError(BindingErrorCode Code, const char* Message)
		{
			return std::unexpected(BindingError{ Code, Message });
		}
	}

	std::expected<void, BindingError> ValidateBindingLayout(const BindingLayoutDesc& Description)
	{
		if (Description.Visibility != ShaderVisibility::Vertex &&
			Description.Visibility != ShaderVisibility::Fragment &&
			Description.Visibility != ShaderVisibility::AllGraphics)
			return MakeError(BindingErrorCode::InvalidLayout, "Binding layout visibility is not supported");

		if (Description.Items.empty())
			return MakeError(BindingErrorCode::InvalidLayout, "Binding layout must contain at least one item");

		if (Description.Items.size() > 16)
			return MakeError(BindingErrorCode::InvalidLayout, "Binding layout exceeds PulseForge's 16-item limit");

		std::array<std::array<bool, 32>, 4> SeenSlots = {};
		for (const BindingLayoutItemDesc& Item : Description.Items)
		{
			if (Item.Type != BindingResourceType::Texture2D && Item.Type != BindingResourceType::TextureCube &&
				Item.Type != BindingResourceType::Sampler &&
				Item.Type != BindingResourceType::ConstantBuffer)
				return MakeError(BindingErrorCode::InvalidLayout, "Binding layout contains an unsupported resource type");

			if (Item.Slot >= SeenSlots[0].size())
				return MakeError(BindingErrorCode::InvalidLayout, "Binding slot must be less than 32");

			const size_t TypeIndex = static_cast<size_t>(Item.Type);
			if (SeenSlots[TypeIndex][Item.Slot])
				return MakeError(BindingErrorCode::InvalidLayout, "Binding layout contains a duplicate resource type and slot");
			SeenSlots[TypeIndex][Item.Slot] = true;
		}

		return {};
	}

	std::expected<void, BindingError> ValidateBindingSet(const BindingSetDesc& Description)
	{
		if (!Description.Layout)
			return MakeError(BindingErrorCode::InvalidSet, "Binding set requires a valid binding layout handle");

		const auto LayoutValidation = ValidateBindingLayout(Description.Layout->GetDescription());
		if (!LayoutValidation)
			return std::unexpected(LayoutValidation.error());

		const auto& LayoutItems = Description.Layout->GetDescription().Items;
		if (Description.Buffers.size() + Description.Textures.size() + Description.Samplers.size() != LayoutItems.size())
			return MakeError(BindingErrorCode::InvalidSet, "Binding set must provide exactly one resource for each layout item");

		std::array<std::array<bool, 32>, 4> SeenSlots = {};
		const auto ValidateSlot = [&SeenSlots](BindingResourceType Type, uint32_t Slot) -> std::expected<void, BindingError>
		{
			if (Slot >= SeenSlots[0].size())
				return MakeError(BindingErrorCode::InvalidSet, "Binding slot must be less than 32");

			const size_t TypeIndex = static_cast<size_t>(Type);
			if (SeenSlots[TypeIndex][Slot])
				return MakeError(BindingErrorCode::InvalidSet, "Binding set contains a duplicate resource type and slot");
			SeenSlots[TypeIndex][Slot] = true;
			return {};
		};

		for (const TextureBindingDesc& TextureBinding : Description.Textures)
		{
			const TextureDesc& TextureDescription = TextureBinding.Resource.get().GetDescription();
			const BindingResourceType ExpectedType = TextureDescription.Dimension == TextureDimension::TextureCube
				? BindingResourceType::TextureCube
				: BindingResourceType::Texture2D;
			if (TextureBinding.Type != ExpectedType)
				return MakeError(BindingErrorCode::InvalidSet, "Texture resource dimension does not match its declared binding type");
			if (TextureBinding.Type != BindingResourceType::Texture2D && TextureBinding.Type != BindingResourceType::TextureCube)
				return MakeError(BindingErrorCode::InvalidSet, "Texture binding type must be Texture2D or TextureCube");
			const auto SlotValidation = ValidateSlot(TextureBinding.Type, TextureBinding.Slot);
			if (!SlotValidation)
				return std::unexpected(SlotValidation.error());
			if (!HasTextureUsage(TextureDescription.Usage, TextureUsage::ShaderResource) ||
				TextureDescription.Format == TextureFormat::Depth32Float)
			{
				return MakeError(BindingErrorCode::InvalidSet, "Texture binding requires a color texture with ShaderResource usage");
			}

			const auto LayoutItem = std::find_if(LayoutItems.begin(), LayoutItems.end(), [&TextureBinding](const auto& Item)
			{
				return Item.Slot == TextureBinding.Slot && Item.Type == TextureBinding.Type;
			});
			if (LayoutItem == LayoutItems.end())
				return MakeError(BindingErrorCode::InvalidSet, "Texture binding dimension does not match its layout item");
		}

		for (const SamplerBindingDesc& SamplerBinding : Description.Samplers)
		{
			const auto SlotValidation = ValidateSlot(BindingResourceType::Sampler, SamplerBinding.Slot);
			if (!SlotValidation)
				return std::unexpected(SlotValidation.error());

			const auto LayoutItem = std::find_if(LayoutItems.begin(), LayoutItems.end(), [&SamplerBinding](const auto& Item)
			{
				return Item.Slot == SamplerBinding.Slot && Item.Type == BindingResourceType::Sampler;
			});
			if (LayoutItem == LayoutItems.end())
				return MakeError(BindingErrorCode::InvalidSet, "Sampler binding does not match a Sampler item in the layout");
		}

		for (const BufferBindingDesc& BufferBinding : Description.Buffers)
		{
			const auto SlotValidation = ValidateSlot(BindingResourceType::ConstantBuffer, BufferBinding.Slot);
			if (!SlotValidation)
				return std::unexpected(SlotValidation.error());

			const auto LayoutItem = std::find_if(LayoutItems.begin(), LayoutItems.end(), [&BufferBinding](const auto& Item)
			{
				return Item.Slot == BufferBinding.Slot && Item.Type == BindingResourceType::ConstantBuffer;
			});
			if (LayoutItem == LayoutItems.end())
				return MakeError(BindingErrorCode::InvalidSet, "Buffer binding does not match a constant-buffer item in the layout");

			if (BufferBinding.Resource.get().GetDescription().Usage != BufferUsage::Constant)
				return MakeError(BindingErrorCode::InvalidSet, "Constant-buffer binding requires a constant-usage PulseForge buffer");
		}

		return {};
	}
}
