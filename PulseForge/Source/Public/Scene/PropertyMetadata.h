#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

namespace PulseForge
{
	class Entity;

	using PropertyValue = std::variant<bool, float, glm::vec3, glm::quat, AssetID, std::optional<AssetID>, std::string>;

	enum class PropertyValueType : uint8_t
	{
		Boolean,
		Float,
		Vector3,
		Quaternion,
		AssetId,
		OptionalAssetId,
		String
	};

	namespace PropertyComponentIds
	{
		inline constexpr std::string_view Transform = "transform";
		inline constexpr std::string_view DirectionalLight = "directionalLight";
		inline constexpr std::string_view PointLight = "pointLight";
		inline constexpr std::string_view SpotLight = "spotLight";
		inline constexpr std::string_view MeshRenderer = "meshRenderer";
		inline constexpr std::string_view Script = "script";
	} // namespace PropertyComponentIds

	namespace PropertyIds
	{
		inline constexpr std::string_view Translation = "translation";
		inline constexpr std::string_view Rotation = "rotation";
		inline constexpr std::string_view Scale = "scale";
		inline constexpr std::string_view Color = "color";
		inline constexpr std::string_view Intensity = "intensity";
		inline constexpr std::string_view Range = "range";
		inline constexpr std::string_view CastShadows = "castShadows";
		inline constexpr std::string_view ShadowDistance = "shadowDistance";
		inline constexpr std::string_view ShadowBias = "shadowBias";
		inline constexpr std::string_view ShadowNormalBias = "shadowNormalBias";
		inline constexpr std::string_view ShadowSoftness = "shadowSoftness";
		inline constexpr std::string_view InnerConeAngleDegrees = "innerConeAngleDegrees";
		inline constexpr std::string_view OuterConeAngleDegrees = "outerConeAngleDegrees";
		inline constexpr std::string_view MaterialAsset = "materialAsset";
		inline constexpr std::string_view Enabled = "enabled";
		inline constexpr std::string_view Asset = "asset";
	} // namespace PropertyIds

	enum class PropertyReferenceKind : uint8_t
	{
		None,
		MeshAsset,
		MaterialAsset,
		ScriptAsset
	};

	struct PropertyDescriptor
	{
		std::string_view ComponentId;
		std::string_view Id;
		std::string_view Label;
		PropertyValueType Type;
		PropertyValue DefaultValue;
		std::string_view Units;
		bool EditorVisible = true;
		bool RuntimeVisible = true;
		std::optional<float> AdvisoryMinimum;
		std::optional<float> AdvisoryMaximum;
		PropertyReferenceKind ReferenceKind = PropertyReferenceKind::None;
		bool Editable = true;

		template <typename T>
		PropertyDescriptor(std::string_view Component, std::string_view Identifier, std::string_view DisplayLabel, T&& Default,
			std::string_view Unit = {}, bool ShowInEditor = true, bool ShowInRuntime = true, std::optional<float> Minimum = {},
			std::optional<float> Maximum = {}, PropertyReferenceKind Reference = PropertyReferenceKind::None, bool CanEdit = true)
			: ComponentId(Component), Id(Identifier), Label(DisplayLabel), Type(ValueTypeFor<T>()), DefaultValue(std::forward<T>(Default)),
			  Units(Unit), EditorVisible(ShowInEditor), RuntimeVisible(ShowInRuntime), AdvisoryMinimum(Minimum), AdvisoryMaximum(Maximum),
			  ReferenceKind(Reference), Editable(CanEdit)
		{
		}

	private:
		template <typename T> static consteval PropertyValueType ValueTypeFor()
		{
			using Value = std::remove_cvref_t<T>;
			if constexpr (std::is_same_v<Value, bool>) return PropertyValueType::Boolean;
			else if constexpr (std::is_same_v<Value, float>) return PropertyValueType::Float;
			else if constexpr (std::is_same_v<Value, glm::vec3>) return PropertyValueType::Vector3;
			else if constexpr (std::is_same_v<Value, glm::quat>) return PropertyValueType::Quaternion;
			else if constexpr (std::is_same_v<Value, AssetID>) return PropertyValueType::AssetId;
			else if constexpr (std::is_same_v<Value, std::optional<AssetID>>) return PropertyValueType::OptionalAssetId;
			else if constexpr (std::is_same_v<Value, std::string>) return PropertyValueType::String;
			else static_assert(!sizeof(Value), "Unsupported PropertyValue type");
		}
	};

	enum class PropertyAccessErrorCode : uint8_t
	{
		UnknownComponent,
		UnknownProperty,
		InvalidEntity,
		MissingComponent,
		TypeMismatch,
		InvalidValue,
		ReadOnlyProperty,
		ComponentRejected
	};

	struct PropertyAccessError
	{
		PropertyAccessErrorCode Code;
		std::string Message;
	};

	[[nodiscard]] PULSEFORGE_API std::span<const PropertyDescriptor> GetPropertyDescriptors() noexcept;
	[[nodiscard]] PULSEFORGE_API const PropertyDescriptor* FindPropertyDescriptor(
		std::string_view ComponentId, std::string_view PropertyId) noexcept;
	[[nodiscard]] PULSEFORGE_API std::expected<PropertyValue, PropertyAccessError> ReadProperty(
		const Entity& Target, std::string_view ComponentId, std::string_view PropertyId);
	[[nodiscard]] PULSEFORGE_API std::expected<void, PropertyAccessError> WriteProperty(
		const Entity& Target, std::string_view ComponentId, std::string_view PropertyId, const PropertyValue& Value);
} // namespace PulseForge
