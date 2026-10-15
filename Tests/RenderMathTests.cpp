#include "TestSupport.h"

#include "Renderer/LocalLighting.h"
#include "Renderer/PointLightShadowMath.h"
#include "Renderer/ScreenSpaceAmbientOcclusion.h"
#include "Renderer/SpotlightShadowMath.h"
#include "Renderer/ToneMapping.h"
#include "Scene/Components/PointLightComponent.h"
#include "Scene/Components/SpotLightComponent.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <span>

namespace
{
	void TestLocalLightingMathAndSelection(TestRunner& Tests)
	{
		using namespace PulseForge;
		static_assert(sizeof(LocalLightingConstants) == 2064);
		PointLightComponent Point;
		SpotLightComponent Spot;
		PF_CHECK(Tests, Point.Validate().has_value() && Spot.Validate().has_value());
		Point.Intensity = 0.0f;
		PF_CHECK(Tests, Point.Validate().has_value());
		Point.Intensity = -1.0f;
		PF_CHECK(Tests, !Point.Validate());
		Point.Intensity = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !Point.Validate());
		Point.Intensity = 1000000.0f;
		Point.Range = 10000.0f;
		PF_CHECK(Tests, Point.Validate().has_value());
		Point.Color.r = -0.01f;
		PF_CHECK(Tests, !Point.Validate());
		Point.Color.r = 1.0f;
		Point.Intensity = 1.0f;
		Point.Range = 0.0f;
		PF_CHECK(Tests, !Point.Validate());
		Point.Range = 1.0f;
		Point.Color.g = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !Point.Validate());
		Spot.OuterConeAngleDegrees = Spot.InnerConeAngleDegrees;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.InnerConeAngleDegrees = 30.0f;
		Spot.OuterConeAngleDegrees = 20.0f;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.InnerConeAngleDegrees = 10.0f;
		Spot.OuterConeAngleDegrees = 90.0f;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.InnerConeAngleDegrees = 0.5f;
		Spot.OuterConeAngleDegrees = 89.5f;
		Spot.Intensity = 0.0f;
		PF_CHECK(Tests, Spot.Validate().has_value());
		Spot.OuterConeAngleDegrees = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !Spot.Validate());
		Spot.OuterConeAngleDegrees = 25.0f;
		Spot.ShadowBias = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !Spot.Validate());
		Spot.ShadowBias = 0.001f;
		Spot.ShadowNormalBias = -0.01f;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.ShadowNormalBias = 0.025f;
		Spot.ShadowSoftness = 4.1f;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.ShadowSoftness = 1.5f;

		const auto Projection = BuildSpotlightShadowProjection(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 10.0f, 30.0f);
		PF_CHECK(Tests, Projection.has_value());
		if (Projection)
		{
			glm::vec3 UvDepth;
			PF_CHECK(Tests, ProjectSpotlightShadowCoordinate(Projection->ViewProjection, { 0.0f, 0.0f, -5.0f }, UvDepth));
			PF_CHECK(Tests, glm::abs(UvDepth.x - 0.5f) < 0.0001f && glm::abs(UvDepth.y - 0.5f) < 0.0001f);
			PF_CHECK(Tests, UvDepth.z > 0.0f && UvDepth.z < 1.0f);
			PF_CHECK(Tests, !ProjectSpotlightShadowCoordinate(Projection->ViewProjection, { 0.0f, 0.0f, 1.0f }, UvDepth));
			PF_CHECK(Tests, !ProjectSpotlightShadowCoordinate(Projection->ViewProjection, { 0.0f, 0.0f, -12.0f }, UvDepth));
		}
		PF_CHECK(Tests, !BuildSpotlightShadowProjection(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 10.0f, 85.0f));
		PF_CHECK(Tests, !BuildSpotlightShadowProjection(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 0.0f), 10.0f, 30.0f));
		PF_CHECK(Tests, !BuildSpotlightShadowProjection(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.01f, 30.0f));
		const auto VerticalProjection = BuildSpotlightShadowProjection(glm::vec3(1.0f), glm::vec3(0.0f, 1.0f, 0.00001f), 10.0f, 45.0f);
		PF_CHECK(Tests, VerticalProjection.has_value());
		PF_CHECK(Tests, VerticalProjection && std::isfinite(VerticalProjection->ViewProjection[0][0]));
		PF_CHECK(Tests, MaxSpotlightShadowMapCount == 4 && SpotlightShadowMapResolution == 1024);
		PF_CHECK(Tests, !Spot.CastShadows);

		SpotlightShadowSlotOwners ShadowOwners{};
		const std::array<SpotlightShadowCandidate, 5> ShadowCandidates = { { { UUID(0, 10), 0 }, { UUID(0, 20), 1 }, { UUID(0, 30), 2 },
			{ UUID(0, 40), 3 }, { UUID(0, 50), 4 } } };
		const auto InitialAssignments = AssignSpotlightShadowSlots(ShadowCandidates, ShadowOwners);
		PF_CHECK(Tests, InitialAssignments[0] == 0 && InitialAssignments[1] == 1 && InitialAssignments[2] == 2 &&
							InitialAssignments[3] == 3 && InitialAssignments[4] == -1);
		const std::array<SpotlightShadowCandidate, 4> ReorderedCandidates = { { { UUID(0, 40), 0 }, { UUID(0, 30), 1 }, { UUID(0, 20), 2 },
			{ UUID(0, 10), 3 } } };
		const auto StableAssignments = AssignSpotlightShadowSlots(ReorderedCandidates, ShadowOwners);
		PF_CHECK(Tests, StableAssignments[0] == 3 && StableAssignments[1] == 2 && StableAssignments[2] == 1 && StableAssignments[3] == 0);
		const std::array<SpotlightShadowCandidate, 4> ReassignedCandidates = { { { UUID(0, 40), 0 }, { UUID(0, 30), 1 }, { UUID(0, 20), 2 },
			{ UUID(0, 60), 3 } } };
		const auto Reassigned = AssignSpotlightShadowSlots(ReassignedCandidates, ShadowOwners);
		PF_CHECK(Tests, Reassigned[0] == 3 && Reassigned[1] == 2 && Reassigned[2] == 1 && Reassigned[3] == 0);
		const auto ClearedAssignments = AssignSpotlightShadowSlots({}, ShadowOwners);
		PF_CHECK(Tests, std::ranges::all_of(ClearedAssignments, [](int32_t Slot) { return Slot == -1; }));
		PF_CHECK(Tests, std::ranges::all_of(ShadowOwners, [](const auto& Owner) { return !Owner.has_value(); }));

		// Over-budget lights are selected by candidate rank every update; slot persistence only applies inside
		// that selected set. A previously waiting light must not retain a slot over a higher-ranked light.
		const std::array<SpotlightShadowCandidate, 5> RankedCandidates = { { { UUID(0, 101), 0 }, { UUID(0, 102), 1 }, { UUID(0, 103), 2 },
			{ UUID(0, 104), 3 }, { UUID(0, 105), 4 } } };
		SpotlightShadowSlotOwners RankedOwners{};
		const auto RankedInitial = AssignSpotlightShadowSlots(RankedCandidates, RankedOwners);
		PF_CHECK(Tests,
			RankedInitial[0] == 0 && RankedInitial[1] == 1 && RankedInitial[2] == 2 && RankedInitial[3] == 3 && RankedInitial[4] == -1);
		const std::array<SpotlightShadowCandidate, 4> WithoutThird = { { { UUID(0, 101), 0 }, { UUID(0, 102), 1 }, { UUID(0, 104), 3 },
			{ UUID(0, 105), 4 } } };
		const auto PromotedFifth = AssignSpotlightShadowSlots(WithoutThird, RankedOwners);
		PF_CHECK(Tests,
			PromotedFifth[0] == 0 && PromotedFifth[1] == 1 && PromotedFifth[2] == -1 && PromotedFifth[3] == 3 && PromotedFifth[4] == 2);
		PF_CHECK(Tests, RankedOwners[2] == UUID(0, 105) && RankedOwners[3] == UUID(0, 104));
		const auto RestoredThird = AssignSpotlightShadowSlots(RankedCandidates, RankedOwners);
		PF_CHECK(Tests,
			RestoredThird[0] == 0 && RestoredThird[1] == 1 && RestoredThird[2] == 2 && RestoredThird[3] == 3 && RestoredThird[4] == -1);
		PF_CHECK(Tests, RankedOwners[2] == UUID(0, 103) && RankedOwners[3] == UUID(0, 104));
		const std::array<SpotlightShadowCandidate, 4> WithoutFirst = { { { UUID(0, 102), 1 }, { UUID(0, 103), 2 }, { UUID(0, 104), 3 },
			{ UUID(0, 105), 4 } } };
		const auto FirstDisabled = AssignSpotlightShadowSlots(WithoutFirst, RankedOwners);
		PF_CHECK(Tests,
			FirstDisabled[0] == -1 && FirstDisabled[1] == 1 && FirstDisabled[2] == 2 && FirstDisabled[3] == 3 && FirstDisabled[4] == 0);
		const auto FirstRestored = AssignSpotlightShadowSlots(RankedCandidates, RankedOwners);
		PF_CHECK(Tests,
			FirstRestored[0] == 0 && FirstRestored[1] == 1 && FirstRestored[2] == 2 && FirstRestored[3] == 3 && FirstRestored[4] == -1);
		PF_CHECK(
			Tests, std::ranges::count_if(RankedOwners, [](const auto& Owner) { return Owner.has_value(); }) == MaxSpotlightShadowMapCount);

		const std::array<SpotlightShadowCandidate, 2> UnderCapacityCandidates = { { { UUID(0, 201), 0 }, { UUID(0, 202), 1 } } };
		SpotlightShadowSlotOwners UnderCapacityOwners{};
		const auto UnderCapacityOn = AssignSpotlightShadowSlots(UnderCapacityCandidates, UnderCapacityOwners);
		PF_CHECK(Tests, UnderCapacityOn[0] >= 0 && UnderCapacityOn[1] >= 0);
		const auto UnderCapacityOff = AssignSpotlightShadowSlots({}, UnderCapacityOwners);
		PF_CHECK(Tests, UnderCapacityOff[0] == -1 && UnderCapacityOff[1] == -1 &&
							std::ranges::none_of(UnderCapacityOwners, [](const auto& Owner) { return Owner.has_value(); }));
		const auto UnderCapacityRestored = AssignSpotlightShadowSlots(UnderCapacityCandidates, UnderCapacityOwners);
		PF_CHECK(Tests, UnderCapacityRestored[0] >= 0 && UnderCapacityRestored[1] >= 0);

		const std::array<SpotlightShadowCandidate, 4> AtCapacityCandidates = { { { UUID(0, 301), 0 }, { UUID(0, 302), 1 },
			{ UUID(0, 303), 2 }, { UUID(0, 304), 3 } } };
		SpotlightShadowSlotOwners AtCapacityOwners{};
		const auto AtCapacityInitial = AssignSpotlightShadowSlots(AtCapacityCandidates, AtCapacityOwners);
		PF_CHECK(Tests, std::ranges::count_if(AtCapacityInitial, [](int32_t Slot) { return Slot >= 0; }) == 4);
		for (size_t DisabledIndex = 0; DisabledIndex < AtCapacityCandidates.size(); ++DisabledIndex)
		{
			std::array<SpotlightShadowCandidate, 3> ReducedCandidates{};
			size_t WriteIndex = 0;
			for (size_t CandidateIndex = 0; CandidateIndex < AtCapacityCandidates.size(); ++CandidateIndex)
				if (CandidateIndex != DisabledIndex) ReducedCandidates[WriteIndex++] = AtCapacityCandidates[CandidateIndex];
			const auto Reduced = AssignSpotlightShadowSlots(ReducedCandidates, AtCapacityOwners);
			PF_CHECK(Tests, std::ranges::count_if(Reduced, [](int32_t Slot) { return Slot >= 0; }) == 3);
			const auto Restored = AssignSpotlightShadowSlots(AtCapacityCandidates, AtCapacityOwners);
			PF_CHECK(Tests, std::ranges::count_if(Restored, [](int32_t Slot) { return Slot >= 0; }) == 4);
			std::array<bool, MaxSpotlightShadowMapCount> UsedSlots{};
			for (size_t CandidateIndex = 0; CandidateIndex < AtCapacityCandidates.size(); ++CandidateIndex)
			{
				const int32_t Slot = Restored[CandidateIndex];
				PF_CHECK(Tests, Slot >= 0 && static_cast<size_t>(Slot) < UsedSlots.size());
				if (Slot >= 0 && static_cast<size_t>(Slot) < UsedSlots.size())
				{
					PF_CHECK(Tests, !UsedSlots[static_cast<size_t>(Slot)]);
					UsedSlots[static_cast<size_t>(Slot)] = true;
				}
			}
		}

		const float AtZero = EvaluateLocalLightRangeAttenuation(0.0f, 10.0f);
		const float AtOne = EvaluateLocalLightRangeAttenuation(1.0f, 10.0f);
		const float AtFive = EvaluateLocalLightRangeAttenuation(5.0f, 10.0f);
		PF_CHECK(Tests, std::isfinite(AtZero) && AtZero > 0.0f);
		PF_CHECK(Tests, AtZero > AtOne && AtOne > AtFive && AtFive > 0.0f);
		PF_CHECK(Tests, EvaluateLocalLightRangeAttenuation(10.0f, 10.0f) == 0.0f);
		PF_CHECK(Tests, EvaluateLocalLightRangeAttenuation(std::numeric_limits<float>::quiet_NaN(), 10.0f) == 0.0f);
		PF_CHECK(Tests, EvaluateLocalLightRangeAttenuation(1.0f, 0.0f) == 0.0f);
		const float InnerCos = std::cos(glm::radians(15.0f));
		const float OuterCos = std::cos(glm::radians(30.0f));
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(1.0f, 15.0f, 30.0f) == 1.0f);
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(InnerCos, 15.0f, 30.0f) > 0.999f);
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(OuterCos, 15.0f, 30.0f) < 0.001f);
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(-1.0f, 15.0f, 30.0f) == 0.0f);
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(std::numeric_limits<float>::quiet_NaN(), 15.0f, 30.0f) == 0.0f);

		std::array<LocalLightRelevance, 4> Lights = { { { UUID{ 0, 4 }, { 0.0f, 0.0f, 0.0f }, glm::vec3(1.0f), 1.0f, 10.0f },
			{ UUID{ 0, 2 }, { 0.0f, 0.0f, 0.0f }, glm::vec3(1.0f), 1.0f, 10.0f },
			{ UUID{ 0, 3 }, { 0.0f, 0.0f, 0.0f }, glm::vec3(1.0f), 0.0f, 10.0f },
			{ UUID{ 0, 1 }, { 2.0f, 0.0f, 0.0f }, glm::vec3(2.0f), 10.0f, 8.0f } } };
		const auto Selected = SelectLocalLightIndices(Lights, glm::vec3(0.0f), 2);
		PF_CHECK(Tests, Selected.size() == 2 && Selected[0] == 3);
		PF_CHECK(Tests, Selected.size() == 2 && Selected[1] == 1);
		PF_CHECK(Tests, SelectLocalLightIndices({}, glm::vec3(0.0f)).empty());
		std::array<LocalLightRelevance, 2> SameEntityTypes = {
			{ { UUID{ 0, 9 }, glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 1.0f, SpotLocalLightTypeOrder },
				{ UUID{ 0, 9 }, glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 1.0f, PointLocalLightTypeOrder } }
		};
		const auto TypeTied = SelectLocalLightIndices(SameEntityTypes, glm::vec3(0.0f));
		PF_CHECK(Tests, TypeTied.size() == 2 && TypeTied[0] == 1 && TypeTied[1] == 0);
		const UUID SharedEntity{ 0, 9 };
		const auto SelectedIndexForType = [&](uint32_t TypeOrder, const auto& Order)
		{
			for (size_t Rank = 0; Rank < Order.size(); ++Rank)
				if (MatchesLocalLightIdentity(SameEntityTypes[Order[Rank]], SharedEntity, TypeOrder)) return Rank;
			return Order.size();
		};
		SameEntityTypes[0].Intensity = 1.0f;
		SameEntityTypes[1].Intensity = 10.0f;
		const auto PointRanksFirst = SelectLocalLightIndices(SameEntityTypes, glm::vec3(0.0f));
		PF_CHECK(Tests, SelectedIndexForType(PointLocalLightTypeOrder, PointRanksFirst) == 0);
		PF_CHECK(Tests, SelectedIndexForType(SpotLocalLightTypeOrder, PointRanksFirst) == 1);
		SameEntityTypes[0].Intensity = 10.0f;
		SameEntityTypes[1].Intensity = 1.0f;
		const auto SpotRanksFirst = SelectLocalLightIndices(SameEntityTypes, glm::vec3(0.0f));
		PF_CHECK(Tests, SelectedIndexForType(SpotLocalLightTypeOrder, SpotRanksFirst) == 0);
		PF_CHECK(Tests, SelectedIndexForType(PointLocalLightTypeOrder, SpotRanksFirst) == 1);

		PointLightComponent SharedPoint;
		SpotLightComponent SharedSpot;
		SharedPoint.CastShadows = true;
		PointShadowSlotOwners SharedPointOwners{};
		SpotlightShadowSlotOwners SharedSpotOwners{};
		const std::array<PointShadowCandidate, 1> SharedPointCandidate = { { { SharedEntity, 1 } } };
		const auto PointOnlySlots = AssignPointShadowSlots(SharedPointCandidate, SharedPointOwners);
		const auto NoSpotSlots = AssignSpotlightShadowSlots({}, SharedSpotOwners);
		PF_CHECK(Tests, SharedPoint.CastShadows && !SharedSpot.CastShadows && PointOnlySlots[1] == 0);
		PF_CHECK(Tests, SharedPointOwners[0] == SharedEntity && !SharedSpotOwners[0] && NoSpotSlots[0] == -1);

		SharedPoint.CastShadows = false;
		SharedSpot.CastShadows = true;
		const auto NoPointSlots = AssignPointShadowSlots({}, SharedPointOwners);
		const std::array<SpotlightShadowCandidate, 1> SharedSpotCandidate = { { { SharedEntity, 0 } } };
		const auto SpotOnlySlots = AssignSpotlightShadowSlots(SharedSpotCandidate, SharedSpotOwners);
		PF_CHECK(Tests, !SharedPoint.CastShadows && SharedSpot.CastShadows && NoPointSlots[0] == -1);
		PF_CHECK(Tests, !SharedPointOwners[0] && SharedSpotOwners[0] == SharedEntity && SpotOnlySlots[0] == 0);

		SharedPoint.CastShadows = true;
		const auto BothPointSlots = AssignPointShadowSlots(SharedPointCandidate, SharedPointOwners);
		const auto BothSpotSlots = AssignSpotlightShadowSlots(SharedSpotCandidate, SharedSpotOwners);
		PF_CHECK(Tests, BothPointSlots[1] == 0 && BothSpotSlots[0] == 0);
		PF_CHECK(Tests, SharedPointOwners[0] == SharedEntity && SharedSpotOwners[0] == SharedEntity);
		PF_CHECK(Tests, MatchesLocalLightIdentity(SameEntityTypes[SpotRanksFirst[0]], SharedEntity, SpotLocalLightTypeOrder));
		PF_CHECK(Tests, MatchesLocalLightIdentity(SameEntityTypes[SpotRanksFirst[1]], SharedEntity, PointLocalLightTypeOrder));
		std::vector<LocalLightRelevance> CapacityLights(MaxLocalLightCount + 1);
		for (size_t Index = 0; Index < CapacityLights.size(); ++Index)
		{
			CapacityLights[Index].Entity = UUID{ 0, Index + 1 };
			CapacityLights[Index].Intensity = 1.0f;
			CapacityLights[Index].Range = 1.0f;
		}
		const auto CapacityBounded = SelectLocalLightIndices(CapacityLights, glm::vec3(0.0f));
		PF_CHECK(Tests, CapacityBounded.size() == MaxLocalLightCount);
		PF_CHECK(Tests, std::find(CapacityBounded.begin(), CapacityBounded.end(), MaxLocalLightCount) == CapacityBounded.end());
	}

	void TestPointLightShadowMath(TestRunner& Tests)
	{
		using namespace PulseForge;
		PointLightComponent Light;
		PF_CHECK(Tests, !Light.CastShadows && Light.Validate());
		Light.CastShadows = true;
		PF_CHECK(Tests, Light.Validate().has_value());
		Light.ShadowBias = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !Light.Validate());
		Light.ShadowBias = 0.001f;
		Light.ShadowNormalBias = -0.1f;
		PF_CHECK(Tests, !Light.Validate());
		Light.ShadowNormalBias = 0.025f;
		Light.ShadowSoftness = MaxPointLightShadowSoftness + 0.1f;
		PF_CHECK(Tests, !Light.Validate());
		Light.ShadowSoftness = 1.5f;

		const auto Projections = BuildPointShadowProjections({ 1.0f, 2.0f, 3.0f }, 10.0f);
		PF_CHECK(Tests, Projections.has_value());
		if (Projections)
		{
			PF_CHECK(Tests, Projections->NearPlane > 0.0f && Projections->NearPlane < Projections->FarPlane);
			PF_CHECK(Tests, Projections->FarPlane == 10.0f);
			for (size_t Face = 0; Face < Projections->Faces.size(); ++Face)
			{
				const auto& Projection = Projections->Faces[Face];
				const glm::vec4 Clip =
					Projection.ViewProjection * glm::vec4(glm::vec3(1.0f, 2.0f, 3.0f) + Projection.Direction * 5.0f, 1.0f);
				PF_CHECK(Tests, std::isfinite(Clip.x) && std::isfinite(Clip.y) && std::isfinite(Clip.z) && Clip.w > 0.0f);
				PF_CHECK(Tests, glm::abs(Clip.x / Clip.w) < 0.0001f && glm::abs(Clip.y / Clip.w) < 0.0001f);
				PF_CHECK(Tests, Clip.z / Clip.w > 0.0f && Clip.z / Clip.w < 1.0f);
			}
		}
		const auto ShortRange = BuildPointShadowProjections(glm::vec3(0.0f), 0.01f);
		PF_CHECK(Tests, ShortRange.has_value() && ShortRange->NearPlane < ShortRange->FarPlane);
		PF_CHECK(Tests, !BuildPointShadowProjections({ std::numeric_limits<float>::infinity(), 0.0f, 0.0f }, 1.0f));
		PF_CHECK(Tests, !BuildPointShadowProjections(glm::vec3(0.0f), 0.0f));
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 1.0f, 0.0f, 0.0f }) == PointShadowCubeFace::PositiveX);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ -1.0f, 0.0f, 0.0f }) == PointShadowCubeFace::NegativeX);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 0.0f, 1.0f, 0.0f }) == PointShadowCubeFace::PositiveY);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 0.0f, -1.0f, 0.0f }) == PointShadowCubeFace::NegativeY);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 0.0f, 0.0f, 1.0f }) == PointShadowCubeFace::PositiveZ);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 0.0f, 0.0f, -1.0f }) == PointShadowCubeFace::NegativeZ);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 1.0f, 1.0f, 0.0f }) == PointShadowCubeFace::PositiveX);
		PF_CHECK(Tests, EncodePointShadowRadialDepth(5.0f, 10.0f) == 0.5f);
		PF_CHECK(Tests, EncodePointShadowRadialDepth(20.0f, 10.0f) == 1.0f);
		PF_CHECK(Tests, EncodePointShadowRadialDepth(std::numeric_limits<float>::quiet_NaN(), 10.0f) == 1.0f);
		PF_CHECK(Tests, IsPointShadowOccluded(0.8f, 0.5f, 0.01f));
		PF_CHECK(Tests, !IsPointShadowOccluded(0.5f, 0.5f, 0.01f));

		const std::array<PointShadowCandidate, 3> Candidates = { { { UUID{ 0, 11 }, 0 }, { UUID{ 0, 12 }, 1 }, { UUID{ 0, 13 }, 2 } } };
		PointShadowSlotOwners Owners{};
		const auto Initial = AssignPointShadowSlots(Candidates, Owners);
		PF_CHECK(Tests, Initial[0] == 0 && Initial[1] == 1 && Initial[2] == -1);
		const std::array<PointShadowCandidate, 2> Replaced = { { { UUID{ 0, 12 }, 1 }, { UUID{ 0, 13 }, 2 } } };
		const auto Reassignment = AssignPointShadowSlots(Replaced, Owners);
		PF_CHECK(Tests, Reassignment[0] == -1 && Reassignment[1] == 1 && Reassignment[2] == 0);
		const auto Restored = AssignPointShadowSlots(Candidates, Owners);
		PF_CHECK(Tests, Restored[0] == 0 && Restored[1] == 1 && Restored[2] == -1);
		PF_CHECK(Tests, Owners[0] == (UUID{ 0, 11 }) && Owners[1] == (UUID{ 0, 12 }));
		const auto Cleared = AssignPointShadowSlots({}, Owners);
		PF_CHECK(Tests, Cleared[0] == -1 && Cleared[1] == -1 && !Owners[0] && !Owners[1]);
		PF_CHECK(Tests, MaxPointLightShadowCount == 2 && PointLightShadowMapResolution == 512);
	}

	void TestToneMappingMathAndOutputEncoding(TestRunner& Tests)
	{
		using namespace PulseForge;
		ToneMappingSettings Settings;
		PF_CHECK(Tests, Settings.ExposureEV == 0.0f);
		PF_CHECK(Tests, IsValidToneMappingSettings(Settings));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -16.0f }));
		PF_CHECK(Tests, Settings.ExposureEV == -16.0f);
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 16.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 0.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -2.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -1.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 1.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 2.0f }));
		for (float Invalid : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
				 -std::numeric_limits<float>::infinity(), -16.01f, 16.01f })
		{
			const ToneMappingSettings Previous = Settings;
			PF_CHECK(Tests, !TryUpdateToneMappingSettings(Settings, { Invalid }));
			PF_CHECK(Tests, Settings.ExposureEV == Previous.ExposureEV);
		}

		const auto Black = ApplyAcesFittedToneMapping({ 0.0f, -1.0f, -10.0f }, 0.0f);
		PF_CHECK(Tests, Black[0] == 0.0f && Black[1] == 0.0f && Black[2] == 0.0f);
		const auto NonFinite = ApplyAcesFittedToneMapping(
			{ std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() },
			0.0f);
		PF_CHECK(Tests, NonFinite[0] == 1.0f && NonFinite[1] == 0.0f && NonFinite[2] == 0.0f);
		const auto Reference = ApplyAcesFittedToneMapping({ 0.18f, 1.0f, 4.0f }, 0.0f);
		PF_CHECK(Tests, std::abs(Reference[0] - 0.2669f) < 0.0002f);
		PF_CHECK(Tests, std::abs(Reference[1] - 0.803797f) < 0.00001f);
		PF_CHECK(Tests, std::abs(Reference[2] - 0.973417f) < 0.00002f);
		const auto NegativeExposure = ApplyAcesFittedToneMapping({ 1.0f, 1.0f, 1.0f }, -1.0f);
		const auto PositiveExposure = ApplyAcesFittedToneMapping({ 1.0f, 1.0f, 1.0f }, 1.0f);
		PF_CHECK(Tests, NegativeExposure[0] < Reference[1] && PositiveExposure[0] > Reference[1]);
		PF_CHECK(Tests, PositiveExposure[0] < 1.0f);
		const auto Large = ApplyAcesFittedToneMapping({ (std::numeric_limits<float>::max)(), 1.0e20f, 65504.0f }, 16.0f);
		PF_CHECK(Tests, Large[0] == 1.0f && Large[1] == 1.0f && Large[2] == 1.0f);
		float Previous = -1.0f;
		for (float Value : { 0.0f, 0.01f, 0.1f, 0.18f, 0.5f, 1.0f, 2.0f, 16.0f, 65504.0f })
		{
			const auto Mapped = ApplyAcesFittedToneMapping({ Value, Value, Value }, 0.0f);
			PF_CHECK(Tests, std::isfinite(Mapped[0]) && Mapped[0] >= 0.0f && Mapped[0] <= 1.0f);
			PF_CHECK(Tests, Mapped[0] >= Previous);
			Previous = Mapped[0];
		}
		PF_CHECK(Tests, LinearToSrgb(0.0f) == 0.0f);
		PF_CHECK(Tests, std::abs(LinearToSrgb(0.0031308f) - 0.0404499f) < 1.0e-5f);
		PF_CHECK(Tests, std::abs(LinearToSrgb(0.04045f) - 0.222205f) < 1.0e-5f);
		PF_CHECK(Tests, LinearToSrgb(1.0f) == 1.0f);
		PF_CHECK(Tests, ShouldShaderEncodeSrgb(ColorTargetFormat::RGBA8_UNorm, OutputColorEncoding::SrgbAttachment));
		PF_CHECK(Tests, !ShouldShaderEncodeSrgb(ColorTargetFormat::RGBA8_Srgb, OutputColorEncoding::UnormAttachment));
		PF_CHECK(Tests, !ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, OutputColorEncoding::SrgbAttachment));
		PF_CHECK(Tests, ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, OutputColorEncoding::UnormAttachment));
		PF_CHECK(Tests, !IsToneMappingBindingCurrent(0, 0));
		PF_CHECK(Tests, IsToneMappingBindingCurrent(5, 5));
		PF_CHECK(Tests, !IsToneMappingBindingCurrent(4, 5));
	}

	void TestScreenSpaceAmbientOcclusionMath(TestRunner& Tests)
	{
		using namespace PulseForge;
		const AmbientOcclusionKernel Kernel = GenerateAmbientOcclusionKernel();
		const AmbientOcclusionKernel RepeatedKernel = GenerateAmbientOcclusionKernel();
		for (size_t Index = 0; Index < Kernel.Samples.size(); ++Index)
		{
			const glm::vec4& Sample = Kernel.Samples[Index];
			PF_CHECK(Tests, std::isfinite(Sample.x) && std::isfinite(Sample.y) && std::isfinite(Sample.z) && Sample.z >= 0.0f);
			PF_CHECK(Tests, glm::length(glm::vec3(Sample)) <= 1.0f + 1.0e-5f);
			PF_CHECK(Tests, Sample == RepeatedKernel.Samples[Index]);
		}
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(1920, 1080) == std::array<uint32_t, 2>{ 960, 540 }));
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(5, 3) == std::array<uint32_t, 2>{ 3, 2 }));
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(1, 1) == std::array<uint32_t, 2>{ 1, 1 }));
		PF_CHECK(Tests,
			CalculateAmbientOcclusionExtent(std::numeric_limits<uint32_t>::max(), 9)[0] == std::numeric_limits<uint32_t>::max() / 2u + 1u);

		const glm::mat4 Projection = glm::perspective(glm::radians(60.0f), 1.6f, 0.1f, 100.0f);
		const glm::mat4 InverseProjection = glm::inverse(Projection);
		for (const glm::vec3 ViewPosition : { glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.5f, -0.3f, -5.0f) })
		{
			const glm::vec4 Clip = Projection * glm::vec4(ViewPosition, 1.0f);
			const glm::vec3 Ndc = glm::vec3(Clip) / Clip.w;
			const glm::vec2 UV{ Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f };
			const auto Reconstructed = ReconstructViewPosition(InverseProjection, UV, Ndc.z);
			PF_CHECK(Tests, Reconstructed && glm::length(*Reconstructed - ViewPosition) < 1.0e-4f);
		}
		PF_CHECK(Tests, !ReconstructViewPosition(InverseProjection, glm::vec2(0.5f), 1.1f));

		for (const glm::vec3 Normal : { glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
				 glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::normalize(glm::vec3(1, 2, 3)) })
		{
			const glm::vec3 Decoded = DecodeViewNormal(EncodeViewNormal(Normal));
			PF_CHECK(Tests, glm::length(Decoded - Normal) < 1.0e-5f);
		}
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(0.0f, 0.6f) == 1.0f);
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(0.6f, 0.6f) == 0.0f);
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(std::numeric_limits<float>::infinity(), 0.6f) == 0.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(1.0f, 0.2f, 0.8f) == 1.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(0.2f, 1.0f, 0.05f) == 1.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(0.2f, 0.0f, 1.0f) < 0.21f);
	}

} // namespace

void RunRenderMathTests(TestRunner& Tests)
{
	TestLocalLightingMathAndSelection(Tests);
	TestPointLightShadowMath(Tests);
	TestToneMappingMathAndOutputEncoding(Tests);
	TestScreenSpaceAmbientOcclusionMath(Tests);
}
