#include "Renderer/SceneRendererLocalLightingState.h"

#include "Core/Log.h"
#include "Renderer/PointLightShadowMath.h"
#include "Renderer/SpotlightShadowMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <span>
#include <utility>

namespace PulseForge
{
	SceneRenderer::LocalLightingState::LocalLightingState()
		: m_Committed(std::make_unique<PreparedData>()), m_Staging(std::make_unique<PreparedData>())
	{
	}

	std::expected<SceneRenderer::LocalLightingState::Plan, SceneRendererError> SceneRenderer::LocalLightingState::BuildPlan(
		const SceneRenderSnapshot* Snapshot)
	{
		m_HasPendingPlan = false;
		m_Pending.SpotlightCandidateCount = 0;
		m_Pending.PointCandidateCount = 0;
		m_Pending.HasSnapshot = Snapshot != nullptr;

		PreparedData& Staged = *m_Staging;
		Staged.SelectedCount = 0;
		Staged.Constants = {};
		Staged.SpotlightMatrices.fill(glm::mat4(1.0f));
		Staged.SpotlightParameters.fill(glm::vec4(0.0f));
		for (auto& FaceMatrices : Staged.PointMatrices)
			FaceMatrices.fill(glm::mat4(1.0f));
		Staged.PointParameters.fill(glm::vec4(0.0f));

		Plan Result;
		if (!Snapshot)
		{
			m_HasPendingPlan = true;
			return Result;
		}

		size_t ActiveLightCount = 0;
		const auto ConsiderLocalLight = [&](const LocalLightRelevance& Relevance, const LocalLightGpuData& Data, UUID Entity,
											const SceneSpotLight* Spot, const ScenePointLight* Point)
		{
			if (Relevance.Intensity <= 0.0f) return;
			++ActiveLightCount;
			const SelectedLight Candidate{ Relevance, Data, Entity, Spot, Point };
			size_t InsertIndex = 0;
			while (InsertIndex < Staged.SelectedCount &&
				   !IsLocalLightMoreRelevant(Candidate.Relevance, Staged.Selected[InsertIndex].Relevance, Snapshot->CameraWorldPosition))
				++InsertIndex;
			if (InsertIndex >= MaxLocalLightCount) return;
			const size_t NewCount = (std::min)(Staged.SelectedCount + 1, MaxLocalLightCount);
			for (size_t Index = NewCount - 1; Index > InsertIndex; --Index)
				Staged.Selected[Index] = Staged.Selected[Index - 1];
			Staged.Selected[InsertIndex] = Candidate;
			Staged.SelectedCount = NewCount;
		};

		for (const ScenePointLight& Light : Snapshot->PointLights)
		{
			LocalLightGpuData Data{};
			Data.PositionRange = glm::vec4(Light.WorldPosition, Light.Range);
			Data.ColorIntensity = glm::vec4(Light.Color, Light.Intensity);
			Data.OuterCosAndType = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
			ConsiderLocalLight({ Light.Entity, Light.WorldPosition, Light.Color, Light.Intensity, Light.Range, PointLocalLightTypeOrder },
				Data, Light.Entity, nullptr, &Light);
		}
		for (const SceneSpotLight& Light : Snapshot->SpotLights)
		{
			LocalLightGpuData Data{};
			Data.PositionRange = glm::vec4(Light.WorldPosition, Light.Range);
			Data.ColorIntensity = glm::vec4(Light.Color, Light.Intensity);
			Data.DirectionInnerCos = glm::vec4(Light.WorldDirection, std::cos(glm::radians(Light.InnerConeAngleDegrees)));
			Data.OuterCosAndType = glm::vec4(std::cos(glm::radians(Light.OuterConeAngleDegrees)), -1.0f, 0.0f, 1.0f);
			ConsiderLocalLight({ Light.Entity, Light.WorldPosition, Light.Color, Light.Intensity, Light.Range, SpotLocalLightTypeOrder },
				Data, Light.Entity, &Light, nullptr);
		}

		if (ActiveLightCount > MaxLocalLightCount && !m_LoggedLocalLightOverflow)
		{
			PF_CORE_WARN("Scene has {0} active local lights; rendering the {1} most camera-relevant lights (UUID and type break ties)",
				ActiveLightCount, MaxLocalLightCount);
			m_LoggedLocalLightOverflow = true;
		}

		for (size_t Index = 0; Index < Staged.SelectedCount; ++Index)
		{
			const SceneSpotLight* Spot = Staged.Selected[Index].Spot;
			if (!Spot || !Spot->CastShadows || Snapshot->Meshes.empty()) continue;
			auto Projection =
				BuildSpotlightShadowProjection(Spot->WorldPosition, Spot->WorldDirection, Spot->Range, Spot->OuterConeAngleDegrees);
			if (!Projection)
			{
				if (Spot->OuterConeAngleDegrees > MaxSpotlightShadowOuterHalfAngleDegrees && !m_LoggedUnsupportedSpotlightCone)
				{
					PF_CORE_WARN("Spotlight shadow projection supports outer half-angles up to {0} degrees; wider spotlights remain "
								 "illuminated without shadows",
						MaxSpotlightShadowOuterHalfAngleDegrees);
					m_LoggedUnsupportedSpotlightCone = true;
				}
				continue;
			}
			m_Pending.SpotlightCandidates[m_Pending.SpotlightCandidateCount++] = { Staged.Selected[Index].Entity,
				static_cast<uint32_t>(Index) };
		}

		for (size_t Index = 0; Index < Staged.SelectedCount; ++Index)
		{
			const ScenePointLight* Point = Staged.Selected[Index].Point;
			if (!Point || !Point->CastShadows || Snapshot->Meshes.empty()) continue;
			auto Projections = BuildPointShadowProjections(Point->WorldPosition, Point->Range);
			if (!Projections)
				return std::unexpected(SceneRendererError{ SceneRendererErrorCode::DrawFailed, {}, {},
					"Could not build point-shadow projections for entity " + Point->Entity.ToString() + ": " +
						Projections.error().Message });
			m_Pending.PointCandidates[m_Pending.PointCandidateCount++] = { Point->Entity, static_cast<uint32_t>(Index) };
		}

		Result.RequiresSpotlightResources = m_Pending.SpotlightCandidateCount > 0;
		Result.RequiresPointShadowResources = m_Pending.PointCandidateCount > 0;
		m_HasPendingPlan = true;
		return Result;
	}

	std::expected<void, SceneRendererError> SceneRenderer::LocalLightingState::Commit()
	{
		if (!m_HasPendingPlan) return std::unexpected(MakeError("Local-light state cannot commit without a successful preparation plan"));
		m_HasPendingPlan = false;

		PreparedData& Staged = *m_Staging;
		Staged.SpotlightOwners = m_Committed->SpotlightOwners;
		Staged.PointOwners = m_Committed->PointOwners;
		if (!m_Pending.HasSnapshot)
		{
			Staged.SpotlightOwners.fill(std::nullopt);
			Staged.PointOwners.fill(std::nullopt);
		}

		const SpotlightShadowSlotAssignments SpotlightAssignments = AssignSpotlightShadowSlots(
			std::span(m_Pending.SpotlightCandidates.data(), m_Pending.SpotlightCandidateCount), Staged.SpotlightOwners);
		for (size_t Index = 0; Index < Staged.SelectedCount; ++Index)
			if (SpotlightAssignments[Index] >= 0)
				Staged.Selected[Index].Data.OuterCosAndType.y = static_cast<float>(SpotlightAssignments[Index]);
		if (m_Pending.SpotlightCandidateCount > MaxSpotlightShadowMapCount && !m_LoggedSpotlightOverflow)
		{
			PF_CORE_WARN(
				"More than {0} selected spotlights request shadows; remaining lights render unshadowed", MaxSpotlightShadowMapCount);
			m_LoggedSpotlightOverflow = true;
		}

		const PointShadowSlotAssignments PointAssignments =
			AssignPointShadowSlots(std::span(m_Pending.PointCandidates.data(), m_Pending.PointCandidateCount), Staged.PointOwners);
		for (size_t Index = 0; Index < Staged.SelectedCount; ++Index)
			if (Staged.Selected[Index].Point && PointAssignments[Index] >= 0)
				Staged.Selected[Index].Data.OuterCosAndType.y = static_cast<float>(PointAssignments[Index]);
		if (m_Pending.PointCandidateCount > MaxPointLightShadowCount && !m_LoggedPointLightOverflow)
		{
			PF_CORE_WARN(
				"More than {0} selected point lights request shadows; remaining lights continue unshadowed", MaxPointLightShadowCount);
			m_LoggedPointLightOverflow = true;
		}

		Staged.Constants.Counts.x = static_cast<uint32_t>(Staged.SelectedCount);
		for (size_t Index = 0; Index < Staged.SelectedCount; ++Index)
			Staged.Constants.Lights[Index] = Staged.Selected[Index].Data;

		for (size_t Slot = 0; Slot < Staged.SpotlightOwners.size(); ++Slot)
		{
			if (!Staged.SpotlightOwners[Slot]) continue;
			const auto SelectedIt = std::find_if(Staged.Selected.begin(),
				Staged.Selected.begin() + static_cast<std::ptrdiff_t>(Staged.SelectedCount),
				[&](const SelectedLight& Light)
				{
					return Light.Spot && MatchesLocalLightIdentity(Light.Relevance, *Staged.SpotlightOwners[Slot], SpotLocalLightTypeOrder);
				});
			if (SelectedIt == Staged.Selected.begin() + static_cast<std::ptrdiff_t>(Staged.SelectedCount) || !SelectedIt->Spot)
				return std::unexpected(MakeError("Spotlight shadow slot no longer identifies a selected spotlight"));
			auto Projection = BuildSpotlightShadowProjection(SelectedIt->Spot->WorldPosition, SelectedIt->Spot->WorldDirection,
				SelectedIt->Spot->Range, SelectedIt->Spot->OuterConeAngleDegrees);
			if (!Projection)
				return std::unexpected(MakeError("Could not build assigned spotlight shadow projection: " + Projection.error().Message));
			Staged.SpotlightMatrices[Slot] = Projection->ViewProjection;
			Staged.SpotlightParameters[Slot] =
				glm::vec4(SelectedIt->Spot->ShadowBias, SelectedIt->Spot->ShadowNormalBias, SelectedIt->Spot->ShadowSoftness, 1.0f);
		}

		for (size_t Slot = 0; Slot < Staged.PointOwners.size(); ++Slot)
		{
			if (!Staged.PointOwners[Slot]) continue;
			const auto SelectedIt = std::find_if(Staged.Selected.begin(),
				Staged.Selected.begin() + static_cast<std::ptrdiff_t>(Staged.SelectedCount), [&](const SelectedLight& Light)
				{ return Light.Point && MatchesLocalLightIdentity(Light.Relevance, *Staged.PointOwners[Slot], PointLocalLightTypeOrder); });
			if (SelectedIt == Staged.Selected.begin() + static_cast<std::ptrdiff_t>(Staged.SelectedCount) || !SelectedIt->Point)
				return std::unexpected(MakeError("Point-shadow slot no longer identifies a selected point light"));
			auto Projections = BuildPointShadowProjections(SelectedIt->Point->WorldPosition, SelectedIt->Point->Range);
			if (!Projections)
				return std::unexpected(MakeError("Could not build assigned point-shadow projections: " + Projections.error().Message));
			for (size_t Face = 0; Face < Projections->Faces.size(); ++Face)
				Staged.PointMatrices[Slot][Face] = Projections->Faces[Face].ViewProjection;
			Staged.PointParameters[Slot] =
				glm::vec4(SelectedIt->Point->ShadowBias, SelectedIt->Point->ShadowNormalBias, SelectedIt->Point->ShadowSoftness, 1.0f);
		}

		m_Committed.swap(m_Staging);
		return {};
	}

	std::span<const SceneRenderer::LocalLightingState::SelectedLight> SceneRenderer::LocalLightingState::SelectedLights() const noexcept
	{
		return { m_Committed->Selected.data(), m_Committed->SelectedCount };
	}

	const SpotlightShadowSlotOwners& SceneRenderer::LocalLightingState::SpotlightSlotOwners() const noexcept
	{
		return m_Committed->SpotlightOwners;
	}

	const PointShadowSlotOwners& SceneRenderer::LocalLightingState::PointSlotOwners() const noexcept
	{
		return m_Committed->PointOwners;
	}

	const std::array<glm::mat4, MaxSpotlightShadowMapCount>& SceneRenderer::LocalLightingState::SpotlightMatrices() const noexcept
	{
		return m_Committed->SpotlightMatrices;
	}

	const std::array<glm::vec4, MaxSpotlightShadowMapCount>& SceneRenderer::LocalLightingState::SpotlightParameters() const noexcept
	{
		return m_Committed->SpotlightParameters;
	}

	const std::array<std::array<glm::mat4, 6>, MaxPointLightShadowCount>& SceneRenderer::LocalLightingState::PointMatrices() const noexcept
	{
		return m_Committed->PointMatrices;
	}

	const std::array<glm::vec4, MaxPointLightShadowCount>& SceneRenderer::LocalLightingState::PointParameters() const noexcept
	{
		return m_Committed->PointParameters;
	}

	const LocalLightingConstants& SceneRenderer::LocalLightingState::Constants() const noexcept
	{
		return m_Committed->Constants;
	}

	SceneRendererError SceneRenderer::LocalLightingState::MakeError(std::string Message)
	{
		return { SceneRendererErrorCode::DrawFailed, {}, {}, std::move(Message) };
	}
} // namespace PulseForge
