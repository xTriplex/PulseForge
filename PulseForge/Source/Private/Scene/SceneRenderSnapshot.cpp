#include "Core/PulseForgePCH.h"
#include "Scene/SceneRenderSnapshot.h"

#include <cmath>

#include <glm/gtc/matrix_inverse.hpp>

namespace PulseForge
{
	std::optional<glm::mat4> BuildNormalTransform(const glm::mat4& Model) noexcept
	{
		const glm::mat3 Linear(Model);
		const float Determinant = glm::determinant(Linear);
		if (!std::isfinite(Determinant) || std::abs(Determinant) <= 1.0e-8f)
			return std::nullopt;
		const glm::mat3 Normal = glm::transpose(glm::inverse(Linear));
		glm::mat4 Result(1.0f);
		for (int Column = 0; Column < 3; ++Column)
			for (int Row = 0; Row < 3; ++Row)
			{
				if (!std::isfinite(Normal[Column][Row]))
					return std::nullopt;
				Result[Column][Row] = Normal[Column][Row];
			}
		return Result;
	}

	namespace
	{
		bool IsFinite(const glm::mat4& Matrix)
		{
			for (int Column = 0; Column < 4; ++Column)
			{
				for (int Row = 0; Row < 4; ++Row)
				{
					if (!std::isfinite(Matrix[Column][Row]))
						return false;
				}
			}
			return true;
		}

		SceneRenderSnapshotError MakeError(
			SceneRenderSnapshotErrorCode Code,
			UUID Entity,
			std::string Message)
		{
			return { Code, Entity, std::move(Message) };
		}

		std::optional<glm::quat> GetWorldRotation(const Entity& Current, size_t Depth = 0)
		{
			if (Depth > 1024)
				return std::nullopt;
			const auto Transform = Current.GetTransform();
			const auto Parent = Current.GetParent();
			if (!Transform || !Parent)
				return std::nullopt;
			const glm::quat Local = Transform->Rotation;
			const float Length = glm::length(Local);
			if (!std::isfinite(Length) || Length <= 1.0e-6f)
				return std::nullopt;
			if (!Parent->has_value())
				return glm::normalize(Local);
			const auto ParentRotation = GetWorldRotation(**Parent, Depth + 1);
			if (!ParentRotation)
				return std::nullopt;
			const glm::quat Result = *ParentRotation * glm::normalize(Local);
			const float ResultLength = glm::length(Result);
			if (!std::isfinite(ResultLength) || ResultLength <= 1.0e-6f)
				return std::nullopt;
			return glm::normalize(Result);
		}

		std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> BuildGeometrySnapshot(
			const Scene& Source,
			const glm::mat4& View,
			const glm::mat4& Projection,
			const glm::mat4& ViewProjection,
			const glm::vec3& CameraWorldPosition,
			float NearClipPlane,
			float FarClipPlane,
			std::optional<UUID> CameraEntity)
		{
			SceneRenderSnapshot Snapshot;
			Snapshot.CameraEntity = CameraEntity;
			Snapshot.ViewProjection = ViewProjection;
			Snapshot.View = View;
			Snapshot.Projection = Projection;
			Snapshot.CameraWorldPosition = CameraWorldPosition;
			Snapshot.NearClipPlane = NearClipPlane;
			Snapshot.FarClipPlane = FarClipPlane;
			Snapshot.HasCameraFrustum = true;

			for (const Entity& Current : Source.GetEntities())
			{
				const auto DirectionalLight = Current.GetDirectionalLight();
				if (!DirectionalLight)
					return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::SceneOperationFailed,
						Current.GetUUID(), DirectionalLight.error().Message));
				if (DirectionalLight->has_value())
				{
					if (Snapshot.DirectionalLight)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::MultipleDirectionalLights,
							Current.GetUUID(), "The direct-light renderer currently supports one directional light per scene"));
					const auto World = Current.GetWorldMatrix();
					if (!World || !IsFinite(*World))
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::InvalidDirectionalLightTransform,
							Current.GetUUID(), "Directional-light world transform must be finite"));
					const glm::vec3 RayDirection = glm::mat3(*World) * glm::vec3(0.0f, 0.0f, -1.0f);
					const float DirectionLength = glm::length(RayDirection);
					if (!std::isfinite(DirectionLength) || DirectionLength <= 1.0e-6f)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::InvalidDirectionalLightTransform,
							Current.GetUUID(), "Directional-light transform produces a zero or non-finite direction"));
					const auto& Light = DirectionalLight->value();
					if (auto Validation = Light.Validate(); !Validation)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::SceneOperationFailed,
							Current.GetUUID(), Validation.error()));
					Snapshot.DirectionalLight = SceneDirectionalLight{
						Current.GetUUID(), RayDirection / DirectionLength, Light.Color, Light.Intensity,
						Light.CastShadows, Light.ShadowDistance, Light.ShadowBias, Light.ShadowNormalBias, Light.ShadowSoftness };
				}

				const auto EnvironmentLight = Current.GetEnvironmentLight();
				if (!EnvironmentLight)
					return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::SceneOperationFailed,
						Current.GetUUID(), EnvironmentLight.error().Message));
				if (EnvironmentLight->has_value())
				{
					if (Snapshot.EnvironmentLight)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::MultipleEnvironmentLights,
							Current.GetUUID(), "The environment-light renderer currently supports one environment per scene"));
					const auto Rotation = GetWorldRotation(Current);
					if (!Rotation)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::InvalidEnvironmentTransform,
							Current.GetUUID(), "Environment world rotation must be finite and non-zero"));
					const auto& Environment = EnvironmentLight->value();
					if (auto Validation = Environment.Validate(); !Validation)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::SceneOperationFailed,
							Current.GetUUID(), Validation.error()));
					Snapshot.EnvironmentLight = SceneEnvironmentLight{
						Current.GetUUID(), Environment.HdrImage, Environment.Intensity, *Rotation };
				}

				const auto PointLight = Current.GetPointLight();
				if (!PointLight)
					return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::SceneOperationFailed,
						Current.GetUUID(), PointLight.error().Message));
				if (PointLight->has_value())
				{
					const auto& Light = PointLight->value();
					if (auto Validation = Light.Validate(); !Validation)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::SceneOperationFailed,
							Current.GetUUID(), Validation.error()));
					const auto World = Current.GetWorldMatrix();
					if (!World || !IsFinite(*World) || !std::isfinite((*World)[3].x) ||
						!std::isfinite((*World)[3].y) || !std::isfinite((*World)[3].z))
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::InvalidLocalLightTransform,
							Current.GetUUID(), "Point-light world position must be finite"));
					Snapshot.PointLights.push_back({ Current.GetUUID(), glm::vec3((*World)[3]), Light.Color,
						Light.Intensity, Light.Range });
				}

				const auto SpotLight = Current.GetSpotLight();
				if (!SpotLight)
					return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::SceneOperationFailed,
						Current.GetUUID(), SpotLight.error().Message));
				if (SpotLight->has_value())
				{
					const auto& Light = SpotLight->value();
					if (auto Validation = Light.Validate(); !Validation)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::SceneOperationFailed,
							Current.GetUUID(), Validation.error()));
					const auto World = Current.GetWorldMatrix();
					const auto Rotation = GetWorldRotation(Current);
					if (!World || !IsFinite(*World) || !Rotation)
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::InvalidLocalLightTransform,
							Current.GetUUID(), "Spot-light world transform must be finite with a valid world rotation"));
					const glm::vec3 Position((*World)[3]);
					const glm::vec3 Direction = glm::normalize(*Rotation * glm::vec3(0.0f, 0.0f, -1.0f));
					if (!std::isfinite(Position.x) || !std::isfinite(Position.y) || !std::isfinite(Position.z) ||
						!std::isfinite(Direction.x) || !std::isfinite(Direction.y) || !std::isfinite(Direction.z))
						return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::InvalidLocalLightTransform,
							Current.GetUUID(), "Spot-light world position or direction is non-finite"));
					Snapshot.SpotLights.push_back({ Current.GetUUID(), Position, Direction, Light.Color,
						Light.Intensity, Light.Range, Light.InnerConeAngleDegrees, Light.OuterConeAngleDegrees,
						Light.CastShadows, Light.ShadowBias, Light.ShadowNormalBias, Light.ShadowSoftness });
				}

				const auto MeshRenderer = Current.GetMeshRenderer();
				if (!MeshRenderer)
				{
					return std::unexpected(MakeError(
						SceneRenderSnapshotErrorCode::SceneOperationFailed,
						Current.GetUUID(),
						MeshRenderer.error().Message));
				}
				if (!MeshRenderer->has_value())
					continue;

				const MeshRendererComponent& Component = MeshRenderer->value();
				if (Component.MeshAsset.IsNil())
				{
					return std::unexpected(MakeError(
						SceneRenderSnapshotErrorCode::SceneOperationFailed,
						Current.GetUUID(),
						"A mesh renderer references a nil mesh asset UUID"));
				}
				if (Component.MaterialAsset && Component.MaterialAsset->IsNil())
				{
					return std::unexpected(MakeError(
						SceneRenderSnapshotErrorCode::SceneOperationFailed,
						Current.GetUUID(),
						"A mesh renderer references a nil material asset UUID"));
				}

				const auto WorldTransform = Current.GetWorldMatrix();
				if (!WorldTransform)
				{
					return std::unexpected(MakeError(
						SceneRenderSnapshotErrorCode::SceneOperationFailed,
						Current.GetUUID(),
						WorldTransform.error().Message));
				}
				if (!IsFinite(*WorldTransform))
				{
					return std::unexpected(MakeError(
						SceneRenderSnapshotErrorCode::InvalidMeshTransform,
						Current.GetUUID(),
						"Mesh entity world transform contains a non-finite matrix value"));
				}

				Snapshot.Meshes.push_back({ Current.GetUUID(), Component.MeshAsset, Component.MaterialAsset, *WorldTransform });
			}
			return Snapshot;
		}
	}

	std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> SceneRenderSnapshotBuilder::Build(
		const Scene& Source,
		UUID CameraEntityIdentifier,
		float AspectRatio)
	{
		if (CameraEntityIdentifier.IsNil())
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::InvalidCameraEntity,
				CameraEntityIdentifier,
				"A non-nil scene camera entity UUID is required"));
		}

		const auto CameraEntity = Source.FindEntity(CameraEntityIdentifier);
		if (!CameraEntity)
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::InvalidCameraEntity,
				CameraEntityIdentifier,
				"The selected scene camera entity does not exist"));
		}

		const auto Camera = CameraEntity->GetCamera();
		if (!Camera)
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::SceneOperationFailed,
				CameraEntityIdentifier,
				Camera.error().Message));
		}
		if (!Camera->has_value())
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::MissingCameraComponent,
				CameraEntityIdentifier,
				"The selected scene camera entity has no camera component"));
		}

		const auto CameraWorld = CameraEntity->GetWorldMatrix();
		if (!CameraWorld)
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::SceneOperationFailed,
				CameraEntityIdentifier,
				CameraWorld.error().Message));
		}
		if (!IsFinite(*CameraWorld))
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::InvalidCameraTransform,
				CameraEntityIdentifier,
				"The selected camera world transform must be finite"));
		}
		const glm::mat4 CameraView = glm::inverse(*CameraWorld);
		if (!IsFinite(CameraView))
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::InvalidCameraTransform,
				CameraEntityIdentifier,
				"The selected camera world transform must be invertible"));
		}

		const auto Projection = Camera->value().GetProjectionMatrix(AspectRatio);
		if (!Projection)
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::InvalidCamera,
				CameraEntityIdentifier,
				Projection.error().Message));
		}

		const glm::mat4 ViewProjection = *Projection * CameraView;
		if (!IsFinite(ViewProjection))
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::InvalidCameraTransform,
				CameraEntityIdentifier,
				"The selected camera produces a non-finite view-projection matrix"));
		}
		return BuildGeometrySnapshot(Source, CameraView, *Projection, ViewProjection, glm::vec3((*CameraWorld)[3]),
			Camera->value().NearClipPlane, Camera->value().FarClipPlane, CameraEntityIdentifier);
	}

	std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> SceneRenderSnapshotBuilder::Build(
		const Scene& Source,
		float AspectRatio)
	{
		std::optional<UUID> PrimaryCamera;
		for (const Entity& Current : Source.GetEntities())
		{
			const auto Camera = Current.GetCamera();
			if (!Camera)
				return std::unexpected(MakeError(
					SceneRenderSnapshotErrorCode::SceneOperationFailed,
					Current.GetUUID(),
					Camera.error().Message));
			if (!Camera->has_value() || !Camera->value().IsPrimary)
				continue;

			if (PrimaryCamera)
			{
				return std::unexpected(MakeError(
					SceneRenderSnapshotErrorCode::MultiplePrimaryCameras,
					Current.GetUUID(),
					"Scene contains more than one primary camera entity"));
			}
			PrimaryCamera = Current.GetUUID();
		}

		if (!PrimaryCamera)
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::MissingPrimaryCamera,
				{},
				"Scene does not contain a primary camera entity"));
		}
		return Build(Source, *PrimaryCamera, AspectRatio);
	}

	std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> SceneRenderSnapshotBuilder::BuildForView(
		const Scene& Source,
		const glm::mat4& ViewProjection,
		const glm::vec3& CameraWorldPosition)
	{
		if (!IsFinite(ViewProjection) || !std::isfinite(CameraWorldPosition.x) ||
			!std::isfinite(CameraWorldPosition.y) || !std::isfinite(CameraWorldPosition.z))
		{
			return std::unexpected(MakeError(
				SceneRenderSnapshotErrorCode::InvalidViewProjection,
				{},
				"The supplied transient view and camera position must contain only finite values"));
		}
		auto Snapshot = BuildForView(Source, glm::mat4(1.0f), ViewProjection, CameraWorldPosition, 0.1f, 1000.0f);
		if (Snapshot)
			Snapshot->HasCameraFrustum = false;
		return Snapshot;
	}

	std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> SceneRenderSnapshotBuilder::BuildForView(
		const Scene& Source,
		const glm::mat4& View,
		const glm::mat4& Projection,
		const glm::vec3& CameraWorldPosition,
		float NearClipPlane,
		float FarClipPlane)
	{
		if (!IsFinite(View) || !IsFinite(Projection) || !std::isfinite(NearClipPlane) ||
			!std::isfinite(FarClipPlane) || NearClipPlane <= 0.0f || FarClipPlane <= NearClipPlane ||
			!std::isfinite(CameraWorldPosition.x) || !std::isfinite(CameraWorldPosition.y) ||
			!std::isfinite(CameraWorldPosition.z))
		{
			return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::InvalidViewProjection, {},
				"Transient render view matrices, position, and clip planes must be finite and valid"));
		}
		const glm::mat4 ViewProjection = Projection * View;
		if (!IsFinite(ViewProjection))
			return std::unexpected(MakeError(SceneRenderSnapshotErrorCode::InvalidViewProjection, {},
				"Transient view-projection matrix must be finite"));
		return BuildGeometrySnapshot(Source, View, Projection, ViewProjection, CameraWorldPosition,
			NearClipPlane, FarClipPlane, std::nullopt);
	}
}
