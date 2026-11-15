#include "Animation/GltfAnimationSampler.h"
#include "Animation/GltfAnimationPlayback.h"
#include "Animation/GltfSkinning.h"
#include "TestSupport.h"

#include <array>
#include <cmath>
#include <limits>
#include <stop_token>
#include <string>

using namespace PulseForge;

namespace
{
	[[nodiscard]] ImportedGltfAnimationChannel MakeChannel(
		GltfAnimationPath Path = GltfAnimationPath::Translation,
		GltfAnimationInterpolation Interpolation = GltfAnimationInterpolation::Linear)
	{
		ImportedGltfAnimationChannel Channel;
		Channel.TargetNodeKey = "node/root";
		Channel.Path = Path;
		Channel.Interpolation = Interpolation;
		Channel.Times = { 0.0f, 2.0f };
		Channel.Values = { { 0.0f, 2.0f, 4.0f, 6.0f }, { 2.0f, 4.0f, 6.0f, 8.0f } };
		return Channel;
	}

	void TestEndpointClampingAndVectorInterpolation(TestRunner& Tests)
	{
		const auto Channel = MakeChannel();
		const auto Before = SampleGltfAnimationChannel(Channel, -1.0f);
		const auto After = SampleGltfAnimationChannel(Channel, 3.0f);
		const auto Middle = SampleGltfAnimationChannel(Channel, 1.0f);
		const std::array<float, 4> ExpectedMiddle = { 1.0f, 3.0f, 5.0f, 7.0f };
		PF_CHECK(Tests, Before && *Before == Channel.Values.front());
		PF_CHECK(Tests, After && *After == Channel.Values.back());
		PF_CHECK(Tests, Middle && *Middle == ExpectedMiddle);
		const auto ExactKey = SampleGltfAnimationChannel(Channel, 2.0f);
		PF_CHECK(Tests, ExactKey && *ExactKey == Channel.Values.back());
	}

	void TestStepUsesPreviousKey(TestRunner& Tests)
	{
		const auto Channel = MakeChannel(GltfAnimationPath::Scale, GltfAnimationInterpolation::Step);
		const auto BeforeSecondKey = SampleGltfAnimationChannel(Channel, 1.999f);
		const auto AtSecondKey = SampleGltfAnimationChannel(Channel, 2.0f);
		PF_CHECK(Tests, BeforeSecondKey && *BeforeSecondKey == Channel.Values.front());
		PF_CHECK(Tests, AtSecondKey && *AtSecondKey == Channel.Values.back());
	}

	void TestRotationShortestPathAndNormalization(TestRunner& Tests)
	{
		// q and -q represent the same orientation; shortest-path slerp must not pass through a zero quaternion.
		auto Channel = MakeChannel(GltfAnimationPath::Rotation);
		Channel.Values = { { 0.0f, 0.0f, 0.0f, 2.0f }, { 0.0f, 0.0f, 0.0f, -3.0f } };
		const auto Middle = SampleGltfAnimationChannel(Channel, 1.0f);
		PF_CHECK(Tests, Middle && std::abs((*Middle)[3] - 1.0f) < 1.0e-5f);
		PF_CHECK(Tests, Middle && std::abs((*Middle)[0]) < 1.0e-5f && std::abs((*Middle)[1]) < 1.0e-5f &&
			std::abs((*Middle)[2]) < 1.0e-5f);

		Channel.Values = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
		const auto QuarterTurn = SampleGltfAnimationChannel(Channel, 1.0f);
		PF_CHECK(Tests, QuarterTurn && std::abs((*QuarterTurn)[2] - std::sqrt(0.5f)) < 1.0e-5f &&
			std::abs((*QuarterTurn)[3] - std::sqrt(0.5f)) < 1.0e-5f);
	}

	void TestMalformedChannelsAndCancellation(TestRunner& Tests)
	{
		auto Channel = MakeChannel();
		Channel.Times.pop_back();
		const auto Mismatched = ValidateGltfAnimationChannel(Channel);
		PF_CHECK(Tests, !Mismatched && Mismatched.error().Code == GltfAnimationSamplingErrorCode::InvalidChannel);

		Channel = MakeChannel();
		Channel.Times[1] = Channel.Times[0];
		const auto DuplicateTime = ValidateGltfAnimationChannel(Channel);
		PF_CHECK(Tests, !DuplicateTime && DuplicateTime.error().Message.find("strictly increasing") != std::string::npos);

		Channel = MakeChannel();
		Channel.Times[0] = std::numeric_limits<float>::quiet_NaN();
		const auto NonFiniteTime = ValidateGltfAnimationChannel(Channel);
		PF_CHECK(Tests, !NonFiniteTime && NonFiniteTime.error().Message.find("non-finite key time") != std::string::npos);

		Channel = MakeChannel();
		Channel.Values[1][2] = std::numeric_limits<float>::infinity();
		const auto NonFiniteValue = ValidateGltfAnimationChannel(Channel);
		PF_CHECK(Tests, !NonFiniteValue && NonFiniteValue.error().Message.find("non-finite sample") != std::string::npos);

		Channel = MakeChannel();
		Channel.Path = static_cast<GltfAnimationPath>(0xff);
		const auto UnsupportedPath = ValidateGltfAnimationChannel(Channel);
		PF_CHECK(Tests, !UnsupportedPath && UnsupportedPath.error().Message.find("unsupported target path") != std::string::npos);

		Channel = MakeChannel();
		Channel.Interpolation = static_cast<GltfAnimationInterpolation>(0xff);
		const auto UnsupportedInterpolation = ValidateGltfAnimationChannel(Channel);
		PF_CHECK(Tests, !UnsupportedInterpolation &&
			UnsupportedInterpolation.error().Message.find("unsupported interpolation") != std::string::npos);

		Channel = MakeChannel(GltfAnimationPath::Rotation);
		Channel.Values[0] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const auto ZeroQuaternion = ValidateGltfAnimationChannel(Channel);
		PF_CHECK(Tests, !ZeroQuaternion && ZeroQuaternion.error().Message.find("nonzero length") != std::string::npos);

		Channel = MakeChannel();
		Channel.Times.resize(MaximumGltfAnimationSamplesPerChannel + 1, 0.0f);
		Channel.Values.resize(MaximumGltfAnimationSamplesPerChannel + 1, { 0.0f, 0.0f, 0.0f, 0.0f });
		const auto TooManySamples = ValidateGltfAnimationChannel(Channel);
		PF_CHECK(Tests, !TooManySamples && TooManySamples.error().Code ==
			GltfAnimationSamplingErrorCode::ResourceLimitExceeded);

		std::stop_source StopSource;
		StopSource.request_stop();
		const auto Cancelled = ValidateGltfAnimationChannel(MakeChannel(), StopSource.get_token());
		PF_CHECK(Tests, !Cancelled && Cancelled.error().Code == GltfAnimationSamplingErrorCode::Cancelled);
		const auto InvalidRequestedTime = SampleGltfAnimationChannel(MakeChannel(),
			std::numeric_limits<float>::quiet_NaN());
		PF_CHECK(Tests, !InvalidRequestedTime &&
			InvalidRequestedTime.error().Code == GltfAnimationSamplingErrorCode::InvalidTime);
	}

	void TestPerInstancePlaybackClock(TestRunner& Tests)
	{
		GltfAnimationPlaybackClock Clock;
		Clock.TimeSeconds = 1.5f;
		GltfAnimationPlaybackClock OtherInstanceClock;
		OtherInstanceClock.TimeSeconds = 0.25f;
		PF_CHECK(Tests, AdvanceGltfAnimationPlayback(Clock, 2.0f, 1.0f));
		PF_CHECK(Tests, std::abs(Clock.TimeSeconds - 0.5f) < 1.0e-6f);
		PF_CHECK(Tests, OtherInstanceClock.TimeSeconds == 0.25f);

		PF_CHECK(Tests, SetGltfAnimationPlaybackSpeed(Clock, -1.0f));
		PF_CHECK(Tests, AdvanceGltfAnimationPlayback(Clock, 2.0f, 1.0f));
		PF_CHECK(Tests, std::abs(Clock.TimeSeconds - 1.5f) < 1.0e-6f);

		Clock.Paused = true;
		PF_CHECK(Tests, AdvanceGltfAnimationPlayback(Clock, 2.0f, 0.5f));
		PF_CHECK(Tests, std::abs(Clock.TimeSeconds - 1.5f) < 1.0e-6f);
		Clock.Paused = false;
		Clock.Looping = false;
		PF_CHECK(Tests, SeekGltfAnimationPlayback(Clock, 2.0f, -5.0f));
		PF_CHECK(Tests, Clock.TimeSeconds == 0.0f);
		PF_CHECK(Tests, AdvanceGltfAnimationPlayback(Clock, 2.0f, 10.0f));
		PF_CHECK(Tests, Clock.TimeSeconds == 0.0f); // Reverse playback clamps at the first key without looping.
		PF_CHECK(Tests, SeekGltfAnimationPlayback(Clock, 2.0f, 5.0f));
		PF_CHECK(Tests, Clock.TimeSeconds == 2.0f);

		std::stop_source StopSource;
		StopSource.request_stop();
		const GltfAnimationPlaybackClock BeforeCancelledAdvance = Clock;
		const auto CancelledAdvance = AdvanceGltfAnimationPlayback(Clock, 2.0f, 0.25f, StopSource.get_token());
		PF_CHECK(Tests, !CancelledAdvance &&
			CancelledAdvance.error().Code == GltfAnimationPlaybackErrorCode::Cancelled);
		PF_CHECK(Tests, Clock.TimeSeconds == BeforeCancelledAdvance.TimeSeconds && Clock.Speed == BeforeCancelledAdvance.Speed);
		const auto CancelledSpeed = SetGltfAnimationPlaybackSpeed(Clock, 3.0f, StopSource.get_token());
		PF_CHECK(Tests, !CancelledSpeed && Clock.Speed == BeforeCancelledAdvance.Speed);
		const auto CancelledSeek = SeekGltfAnimationPlayback(Clock, 2.0f, 1.0f, StopSource.get_token());
		PF_CHECK(Tests, !CancelledSeek && Clock.TimeSeconds == BeforeCancelledAdvance.TimeSeconds);
	}

	void TestPoseSamplingUsesRestValuesAndChannelSampler(TestRunner& Tests)
	{
		ImportedGltfAnimationClip Clip;
		Clip.StableKey = "walk";
		Clip.Name = "Walk";
		Clip.Duration = 2.0f;
		Clip.Channels.push_back(MakeChannel());
		Clip.Channels[0].TargetNodeKey = "root";
		Clip.Channels[0].Values = { { 0.0f, 0.0f, 0.0f, 0.0f }, { 2.0f, 4.0f, 6.0f, 0.0f } };
		ImportedGltfAnimationChannel Rotation = MakeChannel(GltfAnimationPath::Rotation);
		Rotation.TargetNodeKey = "child";
		Rotation.Values = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
		Clip.Channels.push_back(Rotation);

		std::array<GltfAnimationNodePose, 2> RestPose{};
		RestPose[0].NodeKey = "root";
		RestPose[1].NodeKey = "child";
		RestPose[1].Translation = { 10.0f, 11.0f, 12.0f, 0.0f };
		GltfAnimationPlaybackClock Clock;
		Clock.TimeSeconds = 1.0f;
		const auto Pose = SampleGltfAnimationPose(Clip, RestPose, Clock);
		const std::array<float, 4> ExpectedRootTranslation = { 1.0f, 2.0f, 3.0f, 0.0f };
		PF_CHECK(Tests, Pose && Pose->size() == RestPose.size());
		PF_CHECK(Tests, Pose && (*Pose)[0].Translation == ExpectedRootTranslation);
		PF_CHECK(Tests, Pose && (*Pose)[0].Scale == RestPose[0].Scale);
		PF_CHECK(Tests, Pose && (*Pose)[1].Translation == RestPose[1].Translation);
		PF_CHECK(Tests, Pose && std::abs((*Pose)[1].Rotation[2] - std::sqrt(0.5f)) < 1.0e-5f);

		Clip.Channels[1].TargetNodeKey = "missing-node";
		const auto MissingTarget = SampleGltfAnimationPose(Clip, RestPose, Clock);
		PF_CHECK(Tests, !MissingTarget && MissingTarget.error().Code == GltfAnimationPlaybackErrorCode::InvalidClip);

		std::stop_source StopSource;
		StopSource.request_stop();
		const auto CancelledPose = SampleGltfAnimationPose(Clip, RestPose, Clock, StopSource.get_token());
		PF_CHECK(Tests, !CancelledPose && CancelledPose.error().Code == GltfAnimationPlaybackErrorCode::Cancelled);
	}

	void TestCrossfadeProgressAndPoseBlend(TestRunner& Tests)
	{
		GltfAnimationCrossfade Crossfade;
		PF_CHECK(Tests, BeginGltfAnimationCrossfade(Crossfade, 2.0f));
		PF_CHECK(Tests, Crossfade.Active && Crossfade.ElapsedSeconds == 0.0f);
		const auto FirstWeight = AdvanceGltfAnimationCrossfade(Crossfade, 0.5f);
		PF_CHECK(Tests, FirstWeight && std::abs(*FirstWeight - 0.25f) < 1.0e-6f && Crossfade.Active);
		const auto FinalWeight = AdvanceGltfAnimationCrossfade(Crossfade, 1.5f);
		PF_CHECK(Tests, FinalWeight && *FinalWeight == 1.0f && !Crossfade.Active && Crossfade.ElapsedSeconds == 2.0f);

		const GltfAnimationCrossfade BeforeInvalidAdvance = Crossfade;
		const auto NegativeDelta = AdvanceGltfAnimationCrossfade(Crossfade, -0.1f);
		PF_CHECK(Tests, !NegativeDelta && NegativeDelta.error().Code == GltfAnimationPlaybackErrorCode::InvalidClock);
		PF_CHECK(Tests, Crossfade.ElapsedSeconds == BeforeInvalidAdvance.ElapsedSeconds &&
			Crossfade.DurationSeconds == BeforeInvalidAdvance.DurationSeconds && Crossfade.Active == BeforeInvalidAdvance.Active);

		PF_CHECK(Tests, BeginGltfAnimationCrossfade(Crossfade, 0.0f));
		const auto ImmediateWeight = AdvanceGltfAnimationCrossfade(Crossfade, 0.0f);
		PF_CHECK(Tests, ImmediateWeight && *ImmediateWeight == 1.0f && !Crossfade.Active);

		GltfAnimationNodePose From;
		From.NodeKey = "root";
		From.Translation = { 0.0f, 2.0f, 4.0f, 0.0f };
		From.Rotation = { 0.0f, 0.0f, 0.0f, 2.0f };
		GltfAnimationNodePose To = From;
		To.Translation = { 10.0f, 4.0f, 0.0f, 0.0f };
		To.Rotation = { 0.0f, 0.0f, 0.0f, -3.0f };
		const std::array<GltfAnimationNodePose, 1> FromPose = { From };
		const std::array<GltfAnimationNodePose, 1> ToPose = { To };
		const auto Blended = BlendGltfAnimationPoses(FromPose, ToPose, 0.25f);
		const std::array<float, 4> ExpectedTranslation = { 2.5f, 2.5f, 3.0f, 0.0f };
		PF_CHECK(Tests, Blended && (*Blended)[0].NodeKey == "root");
		PF_CHECK(Tests, Blended && (*Blended)[0].Translation == ExpectedTranslation);
		PF_CHECK(Tests, Blended && std::abs((*Blended)[0].Rotation[3] - 1.0f) < 1.0e-6f);
		PF_CHECK(Tests, FromPose[0].Translation == From.Translation && ToPose[0].Translation == To.Translation);

		GltfAnimationNodePose WrongNode = To;
		WrongNode.NodeKey = "other";
		const std::array<GltfAnimationNodePose, 1> MismatchedPose = { WrongNode };
		const auto Mismatched = BlendGltfAnimationPoses(FromPose, MismatchedPose, 0.5f);
		PF_CHECK(Tests, !Mismatched && Mismatched.error().Code == GltfAnimationPlaybackErrorCode::InvalidPose);
		const auto InvalidWeight = BlendGltfAnimationPoses(FromPose, ToPose, 1.1f);
		PF_CHECK(Tests, !InvalidWeight && InvalidWeight.error().Code == GltfAnimationPlaybackErrorCode::InvalidClock);

		std::stop_source StopSource;
		StopSource.request_stop();
		const GltfAnimationCrossfade BeforeCancelledAdvance = Crossfade;
		const auto CancelledAdvance = AdvanceGltfAnimationCrossfade(Crossfade, 0.1f, StopSource.get_token());
		PF_CHECK(Tests, !CancelledAdvance && CancelledAdvance.error().Code == GltfAnimationPlaybackErrorCode::Cancelled);
		PF_CHECK(Tests, Crossfade.ElapsedSeconds == BeforeCancelledAdvance.ElapsedSeconds &&
			Crossfade.Active == BeforeCancelledAdvance.Active);
		const auto CancelledBlend = BlendGltfAnimationPoses(FromPose, ToPose, 0.5f, StopSource.get_token());
		PF_CHECK(Tests, !CancelledBlend && CancelledBlend.error().Code == GltfAnimationPlaybackErrorCode::Cancelled);
	}

	void TestAnimationInstanceTransitionAndInterruption(TestRunner& Tests)
	{
		ImportedGltfSkeletalMesh Mesh;
		const auto AddTranslationClip = [&](std::string Key, float First, float Last)
		{
			ImportedGltfAnimationClip Clip;
			Clip.StableKey = std::move(Key);
			Clip.Duration = 2.0f;
			ImportedGltfAnimationChannel Channel = MakeChannel();
			Channel.TargetNodeKey = "root";
			Channel.Times = { 0.0f, 2.0f };
			Channel.Values = { { First, 0.0f, 0.0f, 0.0f }, { Last, 0.0f, 0.0f, 0.0f } };
			Clip.Channels.push_back(std::move(Channel));
			Mesh.Animations.push_back(std::move(Clip));
		};
		AddTranslationClip("idle", 0.0f, 2.0f);
		AddTranslationClip("walk", 10.0f, 20.0f);
		AddTranslationClip("run", 20.0f, 40.0f);

		GltfAnimationNodePose RestNode;
		RestNode.NodeKey = "root";
		const std::array<GltfAnimationNodePose, 1> RestPose = { RestNode };
		GltfAnimationInstancePlayback FirstActor;
		GltfAnimationInstancePlayback SecondActor;
		PF_CHECK(Tests, PlayGltfAnimationInstance(FirstActor, Mesh, "idle"));
		PF_CHECK(Tests, PlayGltfAnimationInstance(SecondActor, Mesh, "idle"));
		PF_CHECK(Tests, SetGltfAnimationPlaybackSpeed(SecondActor.CurrentClock, 2.0f));
		const auto FirstInitial = AdvanceGltfAnimationInstance(FirstActor, Mesh, RestPose, 0.5f);
		const auto SecondInitial = AdvanceGltfAnimationInstance(SecondActor, Mesh, RestPose, 0.25f);
		PF_CHECK(Tests, FirstInitial && std::abs((*FirstInitial)[0].Translation[0] - 0.5f) < 1.0e-5f);
		PF_CHECK(Tests, SecondInitial && std::abs((*SecondInitial)[0].Translation[0] - 0.5f) < 1.0e-5f);
		PF_CHECK(Tests, FirstActor.CurrentClock.TimeSeconds == 0.5f && SecondActor.CurrentClock.TimeSeconds == 0.5f);

		PF_CHECK(Tests, TransitionGltfAnimationInstance(FirstActor, Mesh, RestPose, "walk", 1.0f));
		const auto FirstHalf = AdvanceGltfAnimationInstance(FirstActor, Mesh, RestPose, 0.5f);
		PF_CHECK(Tests, FirstHalf && std::abs((*FirstHalf)[0].Translation[0] - 6.5f) < 1.0e-5f);
		PF_CHECK(Tests, FirstActor.CurrentClipKey == "idle" && FirstActor.TargetClipKey == "walk");

		PF_CHECK(Tests, TransitionGltfAnimationInstance(FirstActor, Mesh, RestPose, "run", 1.0f));
		const auto InterruptedHalf = AdvanceGltfAnimationInstance(FirstActor, Mesh, RestPose, 0.5f);
		PF_CHECK(Tests, InterruptedHalf && std::abs((*InterruptedHalf)[0].Translation[0] - 15.75f) < 1.0e-5f);
		const auto Completed = AdvanceGltfAnimationInstance(FirstActor, Mesh, RestPose, 0.5f);
		PF_CHECK(Tests, Completed && std::abs((*Completed)[0].Translation[0] - 30.0f) < 1.0e-5f);
		PF_CHECK(Tests, FirstActor.CurrentClipKey == "run" && FirstActor.TargetClipKey.empty() &&
			!FirstActor.Crossfade.Active);
		PF_CHECK(Tests, SecondActor.CurrentClipKey == "idle" && SecondActor.CurrentClock.TimeSeconds == 0.5f);

		const GltfAnimationInstancePlayback BeforeInvalidTransition = SecondActor;
		const auto InvalidTarget = TransitionGltfAnimationInstance(SecondActor, Mesh, RestPose, "missing", 1.0f);
		PF_CHECK(Tests, !InvalidTarget && InvalidTarget.error().Code == GltfAnimationPlaybackErrorCode::InvalidClip);
		PF_CHECK(Tests, SecondActor.CurrentClipKey == BeforeInvalidTransition.CurrentClipKey &&
			SecondActor.CurrentClock.TimeSeconds == BeforeInvalidTransition.CurrentClock.TimeSeconds);
		GltfAnimationInstancePlayback Unstarted;
		const auto InvalidDuration = TransitionGltfAnimationInstance(Unstarted, Mesh, RestPose, "idle", -1.0f);
		PF_CHECK(Tests, !InvalidDuration && InvalidDuration.error().Code == GltfAnimationPlaybackErrorCode::InvalidClock);
		PF_CHECK(Tests, Unstarted.CurrentClipKey.empty() && Unstarted.TargetClipKey.empty());
		const std::array<GltfAnimationNodePose, 1> WrongRestPose = { GltfAnimationNodePose{ .NodeKey = "other" } };
		const GltfAnimationInstancePlayback BeforeWrongRestPose = SecondActor;
		const auto StalePoseTransition = TransitionGltfAnimationInstance(
			SecondActor, Mesh, WrongRestPose, "walk", 1.0f);
		PF_CHECK(Tests, !StalePoseTransition && StalePoseTransition.error().Code == GltfAnimationPlaybackErrorCode::InvalidPose);
		PF_CHECK(Tests, SecondActor.CurrentClipKey == BeforeWrongRestPose.CurrentClipKey &&
			SecondActor.CurrentClock.TimeSeconds == BeforeWrongRestPose.CurrentClock.TimeSeconds &&
			SecondActor.TargetClipKey == BeforeWrongRestPose.TargetClipKey);

		std::stop_source StopSource;
		StopSource.request_stop();
		const auto Cancelled = AdvanceGltfAnimationInstance(FirstActor, Mesh, RestPose, 0.25f, StopSource.get_token());
		PF_CHECK(Tests, !Cancelled && Cancelled.error().Code == GltfAnimationPlaybackErrorCode::Cancelled);
		PF_CHECK(Tests, FirstActor.CurrentClipKey == "run" && FirstActor.CurrentClock.TimeSeconds == 1.0f);
	}

	void TestCpuSkinningCommonVertexLayout(TestRunner& Tests)
	{
		ImportedGltfSkeletalMesh Mesh;
		GltfMeshVertex Vertex;
		Vertex.Position[0] = 1.0f;
		Vertex.Position[1] = 2.0f;
		Vertex.Position[2] = 3.0f;
		Vertex.Normal[0] = 0.0f;
		Vertex.Normal[1] = 0.0f;
		Vertex.Normal[2] = 1.0f;
		Vertex.Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
		Mesh.Geometry.Vertices.push_back(Vertex);
		Mesh.Geometry.Indices = { 0 };
		ImportedGltfSkeletonNode Joint;
		Joint.StableKey = "joint/root";
		Joint.IsJoint = true;
		Mesh.SkeletonNodes.push_back(Joint);
		Mesh.JointNodeIndices = { 0 };
		Mesh.VertexInfluences.resize(1);
		Mesh.VertexInfluences[0].JointIndices[0] = 0;
		Mesh.VertexInfluences[0].Weights[0] = 1.0f;
		const std::array<float, 16> Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

		const auto BindPose = SkinGltfMeshVertices(Mesh, std::span(&Identity, 1));
		PF_CHECK(Tests, BindPose && BindPose->size() == 1);
		PF_CHECK(Tests, BindPose && (*BindPose)[0].Position[0] == 1.0f && (*BindPose)[0].Position[1] == 2.0f &&
			(*BindPose)[0].Position[2] == 3.0f);
		PF_CHECK(Tests, BindPose && (*BindPose)[0].Normal[2] == 1.0f && (*BindPose)[0].Tangent[0] == 1.0f &&
			(*BindPose)[0].Tangent[3] == 1.0f);

		auto Translation = Identity;
		Translation[12] = 5.0f;
		const auto Translated = SkinGltfMeshVertices(Mesh, std::span(&Translation, 1));
		PF_CHECK(Tests, Translated && (*Translated)[0].Position[0] == 6.0f);

		auto NonUniformScale = Identity;
		NonUniformScale[0] = 2.0f;
		NonUniformScale[10] = 0.5f;
		Mesh.Geometry.Vertices[0].Normal[0] = 1.0f;
		Mesh.Geometry.Vertices[0].Normal[2] = 1.0f;
		Mesh.Geometry.Vertices[0].Tangent = { 1.0f, 0.0f, -1.0f, 1.0f };
		const auto Scaled = SkinGltfMeshVertices(Mesh, std::span(&NonUniformScale, 1));
		PF_CHECK(Tests, Scaled && std::abs((*Scaled)[0].Normal[0] - 1.0f / std::sqrt(17.0f)) < 1.0e-5f &&
			std::abs((*Scaled)[0].Normal[2] - 4.0f / std::sqrt(17.0f)) < 1.0e-5f);
		PF_CHECK(Tests, Scaled && std::abs((*Scaled)[0].Tangent[0] - 4.0f / std::sqrt(17.0f)) < 1.0e-5f &&
			std::abs((*Scaled)[0].Tangent[2] + 1.0f / std::sqrt(17.0f)) < 1.0e-5f);

		auto Mirrored = Identity;
		Mirrored[0] = -1.0f;
		Mesh.Geometry.Vertices[0].Normal[0] = 0.0f;
		Mesh.Geometry.Vertices[0].Normal[2] = 1.0f;
		Mesh.Geometry.Vertices[0].Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
		const auto MirroredPose = SkinGltfMeshVertices(Mesh, std::span(&Mirrored, 1));
		PF_CHECK(Tests, MirroredPose && (*MirroredPose)[0].Position[0] == -1.0f);
		PF_CHECK(Tests, MirroredPose && (*MirroredPose)[0].Tangent[0] == -1.0f && (*MirroredPose)[0].Tangent[3] == -1.0f);

		std::array<float, 16> SecondJoint = Identity;
		SecondJoint[12] = 4.0f;
		ImportedGltfSkeletonNode SecondNode = Joint;
		SecondNode.StableKey = "joint/other";
		Mesh.SkeletonNodes.push_back(SecondNode);
		Mesh.JointNodeIndices.push_back(1);
		Mesh.VertexInfluences[0].JointIndices = { 0, 1, 0, 0 };
		Mesh.VertexInfluences[0].Weights = { 0.25f, 0.75f, 0.0f, 0.0f };
		const std::array<std::array<float, 16>, 2> BlendedPalette = { Identity, SecondJoint };
		const auto Blended = SkinGltfMeshVertices(Mesh, BlendedPalette);
		PF_CHECK(Tests, Blended && (*Blended)[0].Position[0] == 4.0f);

		Mesh.VertexInfluences[0].Weights[0] = 0.5f;
		const auto Unnormalized = SkinGltfMeshVertices(Mesh, BlendedPalette);
		PF_CHECK(Tests, !Unnormalized && Unnormalized.error().Code == GltfSkinningErrorCode::InvalidInfluence);
		Mesh.VertexInfluences[0].Weights = { 1.0f, 0.0f, 0.0f, 0.0f };
		Mesh.VertexInfluences[0].JointIndices[0] = 2;
		const auto MissingJoint = SkinGltfMeshVertices(Mesh, BlendedPalette);
		PF_CHECK(Tests, !MissingJoint && MissingJoint.error().Code == GltfSkinningErrorCode::InvalidInfluence);
		Mesh.VertexInfluences[0].JointIndices[0] = 0;
		auto Singular = Identity;
		Singular[0] = 0.0f;
		const std::array<std::array<float, 16>, 2> SingularPalette = { Singular, SecondJoint };
		const auto InvalidPalette = SkinGltfMeshVertices(Mesh, SingularPalette);
		PF_CHECK(Tests, !InvalidPalette && InvalidPalette.error().Code == GltfSkinningErrorCode::InvalidPalette);

		std::stop_source StopSource;
		StopSource.request_stop();
		const auto Cancelled = SkinGltfMeshVertices(Mesh, BlendedPalette, StopSource.get_token());
		PF_CHECK(Tests, !Cancelled && Cancelled.error().Code == GltfSkinningErrorCode::Cancelled);
	}

	void TestMeshLocalJointPaletteConstruction(TestRunner& Tests)
	{
		ImportedGltfSkeletalMesh Mesh;
		Mesh.MeshNodeIndex = 0;
		ImportedGltfSkeletonNode MeshNode;
		MeshNode.StableKey = "node/mesh";
		Mesh.SkeletonNodes.push_back(MeshNode);
		ImportedGltfSkeletonNode Joint;
		Joint.StableKey = "node/joint";
		Joint.IsJoint = true;
		Mesh.SkeletonNodes.push_back(Joint);
		Mesh.JointNodeIndices = { 1 };
		std::array<float, 16> InverseBind = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		InverseBind[12] = -2.0f;
		Mesh.InverseBindMatrices = { InverseBind };
		Mesh.Geometry.Vertices.resize(1);
		Mesh.Geometry.Vertices[0].Position[0] = 1.0f;
		Mesh.Geometry.Indices = { 0 };
		Mesh.VertexInfluences.resize(1);
		Mesh.VertexInfluences[0].JointIndices[0] = 0;
		Mesh.VertexInfluences[0].Weights[0] = 1.0f;

		const auto Translate = [](float X)
		{
			std::array<float, 16> Matrix = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
			Matrix[12] = X;
			return Matrix;
		};
		const std::array<std::array<float, 16>, 2> BindTransforms = { Translate(10.0f), Translate(12.0f) };
		const auto BindPalette = BuildGltfJointPalette(Mesh, BindTransforms);
		PF_CHECK(Tests, BindPalette && BindPalette->size() == 1);
		PF_CHECK(Tests, BindPalette && (*BindPalette)[0] == Translate(0.0f));
		const auto BindSkinned = SkinGltfMeshVertices(Mesh, *BindPalette);
		PF_CHECK(Tests, BindSkinned && (*BindSkinned)[0].Position[0] == 1.0f);

		const std::array<std::array<float, 16>, 2> AnimatedTransforms = { Translate(10.0f), Translate(14.0f) };
		const auto AnimatedPalette = BuildGltfJointPalette(Mesh, AnimatedTransforms);
		PF_CHECK(Tests, AnimatedPalette && (*AnimatedPalette)[0] == Translate(2.0f));
		const auto AnimatedVertices = SkinGltfMeshVertices(Mesh, *AnimatedPalette);
		PF_CHECK(Tests, AnimatedVertices && (*AnimatedVertices)[0].Position[0] == 3.0f);

		const std::array<std::array<float, 16>, 1> TooFewTransforms = { Translate(10.0f) };
		const auto MissingNodeTransform = BuildGltfJointPalette(Mesh, TooFewTransforms);
		PF_CHECK(Tests, !MissingNodeTransform && MissingNodeTransform.error().Code == GltfSkinningErrorCode::InvalidMesh);
		Mesh.SkeletonNodes[1].ParentNodeIndex = 1;
		const auto InvalidHierarchy = BuildGltfJointPalette(Mesh, BindTransforms);
		PF_CHECK(Tests, !InvalidHierarchy && InvalidHierarchy.error().Code == GltfSkinningErrorCode::InvalidMesh);

		std::stop_source StopSource;
		StopSource.request_stop();
		const auto Cancelled = BuildGltfJointPalette(Mesh, BindTransforms, StopSource.get_token());
		PF_CHECK(Tests, !Cancelled && Cancelled.error().Code == GltfSkinningErrorCode::Cancelled);
	}
}

void RunGltfAnimationSamplerTests(TestRunner& Tests)
{
	TestEndpointClampingAndVectorInterpolation(Tests);
	TestStepUsesPreviousKey(Tests);
	TestRotationShortestPathAndNormalization(Tests);
	TestMalformedChannelsAndCancellation(Tests);
	TestPerInstancePlaybackClock(Tests);
	TestPoseSamplingUsesRestValuesAndChannelSampler(Tests);
	TestCrossfadeProgressAndPoseBlend(Tests);
	TestAnimationInstanceTransitionAndInterruption(Tests);
	TestCpuSkinningCommonVertexLayout(Tests);
	TestMeshLocalJointPaletteConstruction(Tests);
}
