#include "Animation/GltfAnimationPlayback.h"
#include "Animation/GltfSkinning.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace PulseForge;

namespace
{
	using Clock = std::chrono::steady_clock;

	ImportedGltfAnimationClip MakeClip(size_t KeyCount)
	{
		ImportedGltfAnimationClip Clip;
		Clip.StableKey = "benchmark/clip";
		Clip.Name = "Sampling benchmark";
		Clip.Duration = 10.0f;
		ImportedGltfAnimationChannel Channel;
		Channel.TargetNodeKey = "root";
		Channel.Times.resize(KeyCount);
		Channel.Values.resize(KeyCount);
		for (size_t Index = 0; Index < KeyCount; ++Index)
		{
			const float Time = Clip.Duration * static_cast<float>(Index) / static_cast<float>(KeyCount - 1);
			Channel.Times[Index] = Time;
			Channel.Values[Index] = { Time, std::sin(Time), 0.0f, 0.0f };
		}
		Clip.Channels.push_back(std::move(Channel));
		return Clip;
	}

	std::array<double, 21> Measure(size_t Iterations, const auto& Operation)
	{
		std::array<double, 21> Milliseconds{};
		for (double& Sample : Milliseconds)
		{
			const auto Started = Clock::now();
			for (size_t Iteration = 0; Iteration < Iterations; ++Iteration) Operation(Iteration);
			Sample = std::chrono::duration<double, std::milli>(Clock::now() - Started).count();
		}
		std::sort(Milliseconds.begin(), Milliseconds.end());
		return Milliseconds;
	}

	void Print(std::string_view Name, size_t Actors, size_t Keys, const std::array<double, 21>& Samples,
		size_t Operations)
	{
		std::cout << "ANIMATION_BENCH name=" << Name << " actors=" << Actors << " keys=" << Keys
			<< " operations_per_sample=" << Operations << " samples=21 p50_ms=" << Samples[10]
			<< " p95_ms=" << Samples[19] << " p99_ms=" << Samples[20] << " min_ms=" << Samples[0]
			<< " max_ms=" << Samples[20] << '\n';
	}
}

int main()
{
	constexpr size_t Iterations = 1000;
	constexpr size_t ActorCount = 16;
	for (const size_t KeyCount : { size_t{ 1000 }, size_t{ 100000 } })
	{
		auto Clip = MakeClip(KeyCount);
		const auto Validation = ValidateGltfAnimationClip(Clip);
		if (!Validation)
		{
			std::cerr << "clip validation failed: " << Validation.error().Message << '\n';
			return 1;
		}
		std::cout << "ANIMATION_BENCH validation_count=1 channels=" << Clip.Channels.size()
			<< " keys=" << KeyCount << "\n";

		std::array<GltfAnimationNodePose, 1> RestPose{};
		RestPose[0].NodeKey = "root";
		const auto Sampling = Measure(Iterations, [&](size_t Index)
		{
			const float Time = static_cast<float>(Index % 1000) * Clip.Duration / 1000.0f;
			const auto Value = SampleGltfAnimationChannel(Clip, *Validation, 0, Time);
			if (!Value) std::abort();
		});
		Print("warmed_channel", 1, KeyCount, Sampling, Iterations);

		std::vector<GltfAnimationPlaybackClock> ActorClocks(ActorCount);
		for (size_t Actor = 0; Actor < ActorCount; ++Actor)
			ActorClocks[Actor].TimeSeconds = static_cast<float>(Actor) * 0.1f;
		const auto Pose = Measure(Iterations, [&](size_t Index)
		{
			for (size_t Actor = 0; Actor < ActorCount; ++Actor)
			{
				ActorClocks[Actor].TimeSeconds = static_cast<float>((Index + Actor) % 1000) * Clip.Duration / 1000.0f;
				const auto Result = SampleGltfAnimationPose(Clip, *Validation, RestPose, ActorClocks[Actor]);
				if (!Result || Result->size() != 1) std::abort();
			}
		});
		Print("shared_product_pose", ActorCount, KeyCount, Pose, Iterations * ActorCount);
	}

	ImportedGltfSkeletalMesh Mesh;
	Mesh.SkeletonNodes.push_back({ "root", "root", -1, true, { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } });
	Mesh.JointNodeIndices.push_back(0);
	Mesh.InverseBindMatrices.push_back({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
	constexpr size_t VertexCount = 10000;
	Mesh.Geometry.Vertices.resize(VertexCount);
	Mesh.VertexInfluences.resize(VertexCount);
	for (auto& Influence : Mesh.VertexInfluences)
	{
		Influence.JointIndices[0] = 0;
		Influence.Weights[0] = 1.0f;
	}
	const std::array<std::array<float, 16>, 1> LocalTransforms{{
		{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } }};
	const auto Palette = BuildGltfJointPalette(Mesh, LocalTransforms);
	if (!Palette) { std::cerr << "palette build failed: " << Palette.error().Message << '\n'; return 1; }
	const auto Skinning = Measure(100, [&](size_t)
	{
		const auto Skinned = SkinGltfMeshVertices(Mesh, *Palette);
		if (!Skinned || Skinned->size() != VertexCount) std::abort();
	});
	Print("skinning_10k_vertices", 1, 0, Skinning, 100);
	return 0;
}
