#include "Scene/PropertyMetadata.h"

#include "Scene/Entity.h"

#include <array>
#include <type_traits>

namespace PulseForge
{
	namespace
	{
		constexpr std::string_view TransformId = PropertyComponentIds::Transform;
		constexpr std::string_view DirectionalLightId = PropertyComponentIds::DirectionalLight;
		constexpr std::string_view PointLightId = PropertyComponentIds::PointLight;
		constexpr std::string_view SpotLightId = PropertyComponentIds::SpotLight;
		constexpr std::string_view MeshRendererId = PropertyComponentIds::MeshRenderer;
		constexpr std::string_view ScriptId = PropertyComponentIds::Script;

		const std::array Descriptors{ PropertyDescriptor{ TransformId, "translation", "Translation", glm::vec3(0.0f), "world units" },
			PropertyDescriptor{ TransformId, "rotation", "Rotation", glm::quat(1.0f, 0.0f, 0.0f, 0.0f), "unit quaternion" },
			PropertyDescriptor{ TransformId, "scale", "Scale", glm::vec3(1.0f), "unitless" },
			PropertyDescriptor{ DirectionalLightId, "color", "Color", glm::vec3(1.0f), "linear RGB" },
			PropertyDescriptor{ DirectionalLightId, "intensity", "Intensity", 1.0f, "unitless", true, true, 0.0f },
			PropertyDescriptor{ DirectionalLightId, "castShadows", "Cast shadows", true, "" },
			PropertyDescriptor{
				DirectionalLightId, "shadowDistance", "Shadow distance", 100.0f, "world units", true, true, 0.0f, 100000.0f },
			PropertyDescriptor{
				DirectionalLightId, "shadowBias", "Shadow bias", 1.0f, "rasterizer depth-bias factor", true, true, 0.0f, 16.0f },
			PropertyDescriptor{
				DirectionalLightId, "shadowNormalBias", "Shadow normal bias", 0.025f, "world units", true, true, 0.0f, 1.0f },
			PropertyDescriptor{
				DirectionalLightId, "shadowSoftness", "Shadow softness", 1.5f, "shadow-map texels", true, true, 0.0f, 8.0f },
			PropertyDescriptor{ PointLightId, "color", "Color", glm::vec3(1.0f), "linear RGB" },
			PropertyDescriptor{ PointLightId, "intensity", "Intensity", 10.0f, "unitless", true, true, 0.0f, 1000000.0f },
			PropertyDescriptor{ PointLightId, "range", "Range", 10.0f, "world units", true, true, 0.0f, 10000.0f },
			PropertyDescriptor{ PointLightId, "castShadows", "Cast shadows", false, "" },
			PropertyDescriptor{
				PointLightId, "shadowBias", "Shadow bias", 0.001f, "normalized radial depth", true, true, 0.0f, MaxPointLightShadowBias },
			PropertyDescriptor{ PointLightId, "shadowNormalBias", "Shadow normal bias", 0.025f, "world units", true, true, 0.0f,
				MaxPointLightShadowNormalBias },
			PropertyDescriptor{
				PointLightId, "shadowSoftness", "Shadow softness", 1.5f, "cubemap texels", true, true, 0.0f, MaxPointLightShadowSoftness },
			PropertyDescriptor{ SpotLightId, "color", "Color", glm::vec3(1.0f), "linear RGB" },
			PropertyDescriptor{ SpotLightId, "intensity", "Intensity", 25.0f, "unitless", true, true, 0.0f, 1000000.0f },
			PropertyDescriptor{ SpotLightId, "range", "Range", 15.0f, "world units", true, true, 0.0f, 10000.0f },
			PropertyDescriptor{ SpotLightId, "innerConeAngleDegrees", "Inner cone angle", 15.0f, "degrees", true, true, 0.5f, 89.5f },
			PropertyDescriptor{ SpotLightId, "outerConeAngleDegrees", "Outer cone angle", 25.0f, "degrees", true, true, 0.5f, 89.5f },
			PropertyDescriptor{ SpotLightId, "castShadows", "Cast shadows", false, "" },
			PropertyDescriptor{ SpotLightId, "shadowBias", "Shadow bias", 0.001f, "light-space depth", true, true, 0.0f, 0.02f },
			PropertyDescriptor{ SpotLightId, "shadowNormalBias", "Shadow normal bias", 0.025f, "world units", true, true, 0.0f, 1.0f },
			PropertyDescriptor{ SpotLightId, "shadowSoftness", "Shadow softness", 1.5f, "shadow-map texels", true, true, 0.0f, 4.0f },
			PropertyDescriptor{ MeshRendererId, "materialAsset", "Material", std::optional<AssetID>{}, "", true, true, {}, {},
				PropertyReferenceKind::MaterialAsset },
			PropertyDescriptor{ MeshRendererId, "meshAsset", "Mesh", AssetID{}, "", true, true, {}, {}, PropertyReferenceKind::MeshAsset },
			PropertyDescriptor{ ScriptId, "enabled", "Enabled", true, "" },
			PropertyDescriptor{ ScriptId, "asset", "Script", AssetID{}, "", true, true, {}, {}, PropertyReferenceKind::ScriptAsset } };

		PropertyAccessError MakeError(PropertyAccessErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		PropertyAccessError SceneErrorToPropertyError(const SceneError& Error)
		{
			return MakeError(Error.Code == SceneErrorCode::InvalidEntity ? PropertyAccessErrorCode::InvalidEntity
																		 : PropertyAccessErrorCode::ComponentRejected,
				Error.Message);
		}

		PropertyAccessError UnknownPropertyError(std::string_view ComponentId, std::string_view PropertyId)
		{
			for (const PropertyDescriptor& Descriptor : Descriptors)
				if (Descriptor.ComponentId == ComponentId)
					return MakeError(PropertyAccessErrorCode::UnknownProperty,
						"Unknown property '" + std::string(PropertyId) + "' on component '" + std::string(ComponentId) + "'");
			return MakeError(PropertyAccessErrorCode::UnknownComponent, "Unknown property component '" + std::string(ComponentId) + "'");
		}

		PropertyAccessError ScriptErrorToPropertyError(const ScriptComponentError& Error)
		{
			return MakeError(PropertyAccessErrorCode::InvalidValue, Error.Message);
		}

		template <typename T>
		std::expected<T, PropertyAccessError> RequireComponent(
			std::expected<std::optional<T>, SceneError> Result, std::string_view ComponentId)
		{
			if (!Result) return std::unexpected(SceneErrorToPropertyError(Result.error()));
			if (!Result->has_value())
				return std::unexpected(MakeError(
					PropertyAccessErrorCode::MissingComponent, "Entity does not have component '" + std::string(ComponentId) + "'"));
			return Result->value();
		}

		template <typename T> std::expected<T, PropertyAccessError> RequireValue(const PropertyValue& Value, std::string_view PropertyId)
		{
			if (const T* TypedValue = std::get_if<T>(&Value)) return *TypedValue;
			return std::unexpected(
				MakeError(PropertyAccessErrorCode::TypeMismatch, "Value type does not match property '" + std::string(PropertyId) + "'"));
		}
	} // namespace

	std::span<const PropertyDescriptor> GetPropertyDescriptors() noexcept
	{
		return Descriptors;
	}

	std::expected<std::vector<DynamicPropertyDescriptor>, PropertyAccessError> GetDynamicPropertyDescriptors(
		const Entity& Target, std::string_view ComponentId)
	{
		if (ComponentId != PropertyComponentIds::ScriptFloat) return std::unexpected(UnknownPropertyError(ComponentId, {}));
		auto Script = RequireComponent(Target.GetScript(), PropertyComponentIds::Script);
		if (!Script) return std::unexpected(Script.error());
		std::vector<DynamicPropertyDescriptor> Results;
		Results.reserve(Script->FloatProperties.size());
		for (const auto& [Identifier, Value] : Script->FloatProperties)
		{
			(void)Value;
			Results.push_back({ std::string(PropertyComponentIds::ScriptFloat), Identifier, Identifier, PropertyValueType::Float, true });
		}
		return Results;
	}

	const PropertyDescriptor* FindPropertyDescriptor(std::string_view ComponentId, std::string_view PropertyId) noexcept
	{
		for (const PropertyDescriptor& Descriptor : Descriptors)
			if (Descriptor.ComponentId == ComponentId && Descriptor.Id == PropertyId) return &Descriptor;
		return nullptr;
	}

	std::expected<PropertyValue, PropertyAccessError> ReadProperty(
		const Entity& Target, std::string_view ComponentId, std::string_view PropertyId)
	{
		if (ComponentId == PropertyComponentIds::ScriptFloat)
		{
			if (!IsValidScriptPropertyIdentifier(PropertyId)) return std::unexpected(UnknownPropertyError(ComponentId, PropertyId));
			auto Script = RequireComponent(Target.GetScript(), PropertyComponentIds::Script);
			if (!Script) return std::unexpected(Script.error());
			const auto Value = Script->FindFloatProperty(PropertyId);
			if (!Value)
				return std::unexpected(MakeError(
					PropertyAccessErrorCode::UnknownProperty, "Script float property '" + std::string(PropertyId) + "' does not exist"));
			return PropertyValue{ *Value };
		}
		if (!FindPropertyDescriptor(ComponentId, PropertyId)) return std::unexpected(UnknownPropertyError(ComponentId, PropertyId));

		if (ComponentId == TransformId)
		{
			const auto Transform = Target.GetTransform();
			if (!Transform) return std::unexpected(SceneErrorToPropertyError(Transform.error()));
			if (PropertyId == "translation") return PropertyValue{ Transform->Translation };
			if (PropertyId == "rotation") return PropertyValue{ Transform->Rotation };
			return PropertyValue{ Transform->Scale };
		}
		if (ComponentId == DirectionalLightId)
		{
			const auto Light = RequireComponent(Target.GetDirectionalLight(), ComponentId);
			if (!Light) return std::unexpected(Light.error());
			if (PropertyId == "color") return PropertyValue{ Light->Color };
			if (PropertyId == "intensity") return PropertyValue{ Light->Intensity };
			if (PropertyId == "castShadows") return PropertyValue{ Light->CastShadows };
			if (PropertyId == "shadowDistance") return PropertyValue{ Light->ShadowDistance };
			if (PropertyId == "shadowBias") return PropertyValue{ Light->ShadowBias };
			if (PropertyId == "shadowNormalBias") return PropertyValue{ Light->ShadowNormalBias };
			return PropertyValue{ Light->ShadowSoftness };
		}
		if (ComponentId == PointLightId)
		{
			const auto Light = RequireComponent(Target.GetPointLight(), ComponentId);
			if (!Light) return std::unexpected(Light.error());
			if (PropertyId == "color") return PropertyValue{ Light->Color };
			if (PropertyId == "intensity") return PropertyValue{ Light->Intensity };
			if (PropertyId == "range") return PropertyValue{ Light->Range };
			if (PropertyId == "castShadows") return PropertyValue{ Light->CastShadows };
			if (PropertyId == "shadowBias") return PropertyValue{ Light->ShadowBias };
			if (PropertyId == "shadowNormalBias") return PropertyValue{ Light->ShadowNormalBias };
			return PropertyValue{ Light->ShadowSoftness };
		}
		if (ComponentId == SpotLightId)
		{
			const auto Light = RequireComponent(Target.GetSpotLight(), ComponentId);
			if (!Light) return std::unexpected(Light.error());
			if (PropertyId == "color") return PropertyValue{ Light->Color };
			if (PropertyId == "intensity") return PropertyValue{ Light->Intensity };
			if (PropertyId == "range") return PropertyValue{ Light->Range };
			if (PropertyId == "innerConeAngleDegrees") return PropertyValue{ Light->InnerConeAngleDegrees };
			if (PropertyId == "outerConeAngleDegrees") return PropertyValue{ Light->OuterConeAngleDegrees };
			if (PropertyId == "castShadows") return PropertyValue{ Light->CastShadows };
			if (PropertyId == "shadowBias") return PropertyValue{ Light->ShadowBias };
			if (PropertyId == "shadowNormalBias") return PropertyValue{ Light->ShadowNormalBias };
			return PropertyValue{ Light->ShadowSoftness };
		}
		if (ComponentId == MeshRendererId)
		{
			const auto Mesh = RequireComponent(Target.GetMeshRenderer(), ComponentId);
			if (!Mesh) return std::unexpected(Mesh.error());
			return PropertyId == PropertyIds::MeshAsset ? PropertyValue{ Mesh->MeshAsset } : PropertyValue{ Mesh->MaterialAsset };
		}
		const auto Script = RequireComponent(Target.GetScript(), ComponentId);
		if (!Script) return std::unexpected(Script.error());
		return PropertyId == "enabled" ? PropertyValue{ Script->Enabled } : PropertyValue{ Script->ScriptAsset };
	}

	std::expected<void, PropertyAccessError> WriteProperty(
		const Entity& Target, std::string_view ComponentId, std::string_view PropertyId, const PropertyValue& Value)
	{
		const PropertyDescriptor* Descriptor = FindPropertyDescriptor(ComponentId, PropertyId);
		if (ComponentId == PropertyComponentIds::ScriptFloat)
		{
			if (!IsValidScriptPropertyIdentifier(PropertyId)) return std::unexpected(UnknownPropertyError(ComponentId, PropertyId));
			auto Typed = RequireValue<float>(Value, PropertyId);
			if (!Typed) return std::unexpected(Typed.error());
			auto Current = RequireComponent(Target.GetScript(), PropertyComponentIds::Script);
			if (!Current) return std::unexpected(Current.error());
			if (!Current->FloatProperties.contains(PropertyId))
				return std::unexpected(MakeError(
					PropertyAccessErrorCode::UnknownProperty, "Script float property '" + std::string(PropertyId) + "' does not exist"));
			if (auto Updated = Current->SetFloatProperty(PropertyId, *Typed); !Updated)
				return std::unexpected(ScriptErrorToPropertyError(Updated.error()));
			const auto Result = Target.SetScript(*Current);
			return Result ? std::expected<void, PropertyAccessError>{} : std::unexpected(SceneErrorToPropertyError(Result.error()));
		}
		if (!Descriptor) return std::unexpected(UnknownPropertyError(ComponentId, PropertyId));
		if (!Descriptor->Editable)
			return std::unexpected(MakeError(PropertyAccessErrorCode::ReadOnlyProperty,
				"Property '" + std::string(PropertyId) + "' on component '" + std::string(ComponentId) + "' is read-only"));

		const auto Reject = [](const SceneError& Error) -> std::expected<void, PropertyAccessError>
		{ return std::unexpected(SceneErrorToPropertyError(Error)); };
		if (ComponentId == TransformId)
		{
			auto Current = Target.GetTransform();
			if (!Current) return Reject(Current.error());
			if (PropertyId == "translation")
			{
				auto Typed = RequireValue<glm::vec3>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Current->Translation = *Typed;
			}
			else if (PropertyId == "rotation")
			{
				auto Typed = RequireValue<glm::quat>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Current->Rotation = *Typed;
			}
			else
			{
				auto Typed = RequireValue<glm::vec3>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Current->Scale = *Typed;
			}
			const auto Result = Target.SetTransform(*Current);
			return Result ? std::expected<void, PropertyAccessError>{} : Reject(Result.error());
		}

		if (ComponentId == MeshRendererId)
		{
			auto Current = RequireComponent(Target.GetMeshRenderer(), ComponentId);
			if (!Current) return std::unexpected(Current.error());
			if (PropertyId == PropertyIds::MeshAsset)
			{
				auto Typed = RequireValue<AssetID>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Current->MeshAsset = *Typed;
			}
			else
			{
				auto Typed = RequireValue<std::optional<AssetID>>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Current->MaterialAsset = *Typed;
			}
			const auto Result = Target.SetMeshRenderer(*Current);
			return Result ? std::expected<void, PropertyAccessError>{} : Reject(Result.error());
		}

		if (ComponentId == ScriptId)
		{
			auto Current = RequireComponent(Target.GetScript(), ComponentId);
			if (!Current) return std::unexpected(Current.error());
			if (PropertyId == "enabled")
			{
				auto Typed = RequireValue<bool>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Current->Enabled = *Typed;
			}
			else
			{
				auto Typed = RequireValue<AssetID>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Current->ScriptAsset = *Typed;
			}
			const auto Result = Target.SetScript(*Current);
			return Result ? std::expected<void, PropertyAccessError>{} : Reject(Result.error());
		}

		if (ComponentId == DirectionalLightId)
		{
			auto Current = RequireComponent(Target.GetDirectionalLight(), ComponentId);
			if (!Current) return std::unexpected(Current.error());
			auto Apply = [&](auto& Field) -> std::expected<void, PropertyAccessError>
			{
				auto Typed = RequireValue<std::remove_cvref_t<decltype(Field)>>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Field = *Typed;
				const auto Result = Target.SetDirectionalLight(*Current);
				return Result ? std::expected<void, PropertyAccessError>{} : Reject(Result.error());
			};
			if (PropertyId == "color") return Apply(Current->Color);
			if (PropertyId == "intensity") return Apply(Current->Intensity);
			if (PropertyId == "castShadows") return Apply(Current->CastShadows);
			if (PropertyId == "shadowDistance") return Apply(Current->ShadowDistance);
			if (PropertyId == "shadowBias") return Apply(Current->ShadowBias);
			if (PropertyId == "shadowNormalBias") return Apply(Current->ShadowNormalBias);
			return Apply(Current->ShadowSoftness);
		}

		if (ComponentId == PointLightId)
		{
			auto Current = RequireComponent(Target.GetPointLight(), ComponentId);
			if (!Current) return std::unexpected(Current.error());
			auto Apply = [&](auto& Field) -> std::expected<void, PropertyAccessError>
			{
				auto Typed = RequireValue<std::remove_cvref_t<decltype(Field)>>(Value, PropertyId);
				if (!Typed) return std::unexpected(Typed.error());
				Field = *Typed;
				const auto Result = Target.SetPointLight(*Current);
				return Result ? std::expected<void, PropertyAccessError>{} : Reject(Result.error());
			};
			if (PropertyId == "color") return Apply(Current->Color);
			if (PropertyId == "intensity") return Apply(Current->Intensity);
			if (PropertyId == "range") return Apply(Current->Range);
			if (PropertyId == "castShadows") return Apply(Current->CastShadows);
			if (PropertyId == "shadowBias") return Apply(Current->ShadowBias);
			if (PropertyId == "shadowNormalBias") return Apply(Current->ShadowNormalBias);
			return Apply(Current->ShadowSoftness);
		}

		auto Current = RequireComponent(Target.GetSpotLight(), ComponentId);
		if (!Current) return std::unexpected(Current.error());
		auto Apply = [&](auto& Field) -> std::expected<void, PropertyAccessError>
		{
			auto Typed = RequireValue<std::remove_cvref_t<decltype(Field)>>(Value, PropertyId);
			if (!Typed) return std::unexpected(Typed.error());
			Field = *Typed;
			const auto Result = Target.SetSpotLight(*Current);
			return Result ? std::expected<void, PropertyAccessError>{} : Reject(Result.error());
		};
		if (PropertyId == "color") return Apply(Current->Color);
		if (PropertyId == "intensity") return Apply(Current->Intensity);
		if (PropertyId == "range") return Apply(Current->Range);
		if (PropertyId == "innerConeAngleDegrees") return Apply(Current->InnerConeAngleDegrees);
		if (PropertyId == "outerConeAngleDegrees") return Apply(Current->OuterConeAngleDegrees);
		if (PropertyId == "castShadows") return Apply(Current->CastShadows);
		if (PropertyId == "shadowBias") return Apply(Current->ShadowBias);
		if (PropertyId == "shadowNormalBias") return Apply(Current->ShadowNormalBias);
		return Apply(Current->ShadowSoftness);
	}

	std::expected<void, PropertyAccessError> AddScriptFloatProperty(const Entity& Target, std::string_view AuthoredId, float Value)
	{
		if (!IsValidScriptPropertyIdentifier(AuthoredId))
			return std::unexpected(MakeError(PropertyAccessErrorCode::InvalidValue, "Invalid authored script float property ID"));
		auto Current = RequireComponent(Target.GetScript(), PropertyComponentIds::Script);
		if (!Current) return std::unexpected(Current.error());
		if (Current->FloatProperties.contains(AuthoredId))
			return std::unexpected(MakeError(PropertyAccessErrorCode::InvalidValue, "Script float property already exists"));
		if (auto Updated = Current->SetFloatProperty(AuthoredId, Value); !Updated)
			return std::unexpected(ScriptErrorToPropertyError(Updated.error()));
		const auto Result = Target.SetScript(*Current);
		return Result ? std::expected<void, PropertyAccessError>{} : std::unexpected(SceneErrorToPropertyError(Result.error()));
	}

	std::expected<void, PropertyAccessError> RemoveScriptFloatProperty(const Entity& Target, std::string_view AuthoredId)
	{
		if (!IsValidScriptPropertyIdentifier(AuthoredId))
			return std::unexpected(MakeError(PropertyAccessErrorCode::InvalidValue, "Invalid authored script float property ID"));
		auto Current = RequireComponent(Target.GetScript(), PropertyComponentIds::Script);
		if (!Current) return std::unexpected(Current.error());
		if (auto Updated = Current->RemoveFloatProperty(AuthoredId); !Updated)
			return std::unexpected(ScriptErrorToPropertyError(Updated.error()));
		const auto Result = Target.SetScript(*Current);
		return Result ? std::expected<void, PropertyAccessError>{} : std::unexpected(SceneErrorToPropertyError(Result.error()));
	}
} // namespace PulseForge
