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

		std::expected<SceneRenderSnapshot, SceneRenderSnapshotError> BuildGeometrySnapshot(
			const Scene& Source,
			const glm::mat4& ViewProjection,
			const glm::vec3& CameraWorldPosition,
			std::optional<UUID> CameraEntity)
		{
			SceneRenderSnapshot Snapshot;
			Snapshot.CameraEntity = CameraEntity;
			Snapshot.ViewProjection = ViewProjection;
			Snapshot.CameraWorldPosition = CameraWorldPosition;

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
						Current.GetUUID(), RayDirection / DirectionLength, Light.Color, Light.Intensity };
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
		return BuildGeometrySnapshot(Source, ViewProjection, glm::vec3((*CameraWorld)[3]), CameraEntityIdentifier);
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
		return BuildGeometrySnapshot(Source, ViewProjection, CameraWorldPosition, std::nullopt);
	}
}
