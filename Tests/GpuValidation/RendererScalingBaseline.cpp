#include "Core/Application.h"
#include "Core/Log.h"
#include "Assets/Project.h"
#include "Assets/SceneAssetService.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PulseForge::GpuValidation
{
	namespace
	{
		constexpr size_t WarmupFrameCount = 45;
		constexpr auto MaximumWarmupDuration = std::chrono::seconds(90);
		constexpr size_t TimedFrameCount = 120;
		constexpr size_t MaximumDrainFrameCount = 180;
		constexpr uint32_t OutputWidth = 1280;
		constexpr uint32_t OutputHeight = 720;
		constexpr size_t DefaultAdditionalInstanceCount = 256;
		constexpr uint64_t GeneratedEntityUUIDHigh = 0x4732375343414c45ull;

		enum class Phase
		{
			Warmup,
			Measure,
			Drain,
			RenderOutput,
			ReadOutput,
			Complete
		};

		[[nodiscard]] std::string EscapeJson(std::string_view Value)
		{
			std::string Result;
			for (const char Character : Value)
			{
				switch (Character)
				{
					case '"': Result += "\\\""; break;
					case '\\': Result += "\\\\"; break;
					case '\n': Result += "\\n"; break;
					case '\r': Result += "\\r"; break;
					case '\t': Result += "\\t"; break;
					default:
						if (static_cast<unsigned char>(Character) >= 0x20) Result += Character;
						break;
				}
			}
			return Result;
		}

		[[nodiscard]] double PercentileMilliseconds(const std::vector<uint64_t>& Samples, double Percentile)
		{
			if (Samples.empty()) return 0.0;
			std::vector<uint64_t> Sorted = Samples;
			std::sort(Sorted.begin(), Sorted.end());
			const size_t Index = std::min(Sorted.size() - 1,
				static_cast<size_t>(std::ceil(Percentile * static_cast<double>(Sorted.size()))) - 1);
			return static_cast<double>(Sorted[Index]) / 1'000'000.0;
		}

		[[nodiscard]] std::string PercentilesJson(const std::vector<uint64_t>& Samples)
		{
			if (Samples.empty()) return "null";
			std::ostringstream Stream;
			Stream.precision(6);
			Stream << "{\"sample_count\":" << Samples.size()
				   << ",\"p50_ms\":" << PercentileMilliseconds(Samples, 0.50)
				   << ",\"p95_ms\":" << PercentileMilliseconds(Samples, 0.95)
				   << ",\"p99_ms\":" << PercentileMilliseconds(Samples, 0.99) << '}';
			return Stream.str();
		}

		[[nodiscard]] uint64_t HashReadback(const TextureReadback& Readback) noexcept
		{
			uint64_t Hash = 14695981039346656037ull;
			for (const std::byte Pixel : Readback.Pixels)
			{
				Hash ^= std::to_integer<uint8_t>(Pixel);
				Hash *= 1099511628211ull;
			}
			for (const uint32_t Value : { Readback.Width, Readback.Height, static_cast<uint32_t>(Readback.Format) })
			{
				for (size_t ByteIndex = 0; ByteIndex < sizeof(Value); ++ByteIndex)
				{
					Hash ^= static_cast<uint8_t>((Value >> (ByteIndex * 8)) & 0xffu);
					Hash *= 1099511628211ull;
				}
			}
			return Hash;
		}

		class ScalingLayer final : public Layer
		{
		public:
			ScalingLayer(size_t AdditionalInstances, std::filesystem::path OutputPath)
				: Layer("G27 renderer scaling baseline"),
				  m_AdditionalInstances(AdditionalInstances),
				  m_OutputPath(std::move(OutputPath))
			{
			}

			void OnAttach() override
			{
				try
				{
					auto& App = Application::Get();
					auto Opened = Project::Open(std::filesystem::path(PF_PBR_DEMO_PROJECT_FILE));
					if (!Opened) throw std::runtime_error("PBR project open failed: " + Opened.error().Message);
					m_Project.emplace(std::move(*Opened));
					if (!m_Project->GetDescription().StartScene)
						throw std::runtime_error("PBR project has no startup scene");
					m_SceneAssetID = *m_Project->GetDescription().StartScene;
					if (const auto Loaded = SceneAssetService::Load(
							m_SceneAssetID, m_Project->GetRootPath(), m_Project->GetAssetRegistry(), m_Scene); !Loaded)
						throw std::runtime_error("PBR startup scene load failed: " + Loaded.error().Message);

					auto Renderer = SceneRenderer::Create(
						App, *m_Project, std::filesystem::path(PF_GPU_VALIDATION_SCENE_SHADER_DIRECTORY));
					if (!Renderer) throw std::runtime_error("SceneRenderer creation failed: " + Renderer.error().Message);
					m_Renderer = std::move(*Renderer);
					FindSourceMeshAndCreateWorkload();
					const auto [Width, Height] = App.GetOutputFramebufferSize();
					if (Width > 0 && Height > 0)
					{
						m_Width = Width;
						m_Height = Height;
					}
					RequirePreparedScene();
					m_WarmupStartedAt = std::chrono::steady_clock::now();
				}
				catch (const std::exception& Exception)
				{
					Fail(Exception.what());
				}
			}

			void OnUpdate(Timestep) override
			{
				if (!m_Succeeded || m_Phase == Phase::Complete) return;
				try
				{
					auto& App = Application::Get();
					CollectGpuTimings(App);
					switch (m_Phase)
					{
						case Phase::Warmup:
							RequirePreparedScene();
							if (m_Renderer->IsAssetPreparationPending())
								m_ReadyWarmupFrames = 0;
							else
								++m_ReadyWarmupFrames;
							if (std::chrono::steady_clock::now() - m_WarmupStartedAt > MaximumWarmupDuration)
								throw std::runtime_error("PBR scene assets did not become ready before the warmup limit");
							if (m_ReadyWarmupFrames >= WarmupFrameCount)
							{
								m_Phase = Phase::Measure;
							}
							break;

						case Phase::Measure:
						{
							if (m_Renderer->IsAssetPreparationPending())
								throw std::runtime_error("PBR scene assets became pending during the measured workload");
							if (m_GpuSessionID == 0)
							{
								if (!App.StartDiagnosticsCapture())
									throw std::runtime_error("Could not start the diagnostic/GPU timing capture");
								m_GpuSessionID = App.GetDiagnosticsCaptureStatus().SessionId;
							}
							const auto SnapshotStart = std::chrono::steady_clock::now();
							auto Snapshot = SceneRenderSnapshotBuilder::Build(m_Scene, m_SceneAspectRatio);
							const auto SnapshotEnd = std::chrono::steady_clock::now();
							if (!Snapshot) throw std::runtime_error(Snapshot.error().Message);
							const auto RendererStart = std::chrono::steady_clock::now();
							const auto Result = m_Renderer->PrepareSnapshotForValidation(std::move(Snapshot));
							const auto RendererEnd = std::chrono::steady_clock::now();
							if (!Result) throw std::runtime_error(Result.error().Message);
							m_SnapshotBuildSamples.push_back(static_cast<uint64_t>(
								std::chrono::duration_cast<std::chrono::nanoseconds>(SnapshotEnd - SnapshotStart).count()));
							m_RendererPrepareSamples.push_back(static_cast<uint64_t>(
								std::chrono::duration_cast<std::chrono::nanoseconds>(RendererEnd - RendererStart).count()));
							m_CpuSamples.push_back(static_cast<uint64_t>(
								std::chrono::duration_cast<std::chrono::nanoseconds>(RendererEnd - SnapshotStart).count()));
							if (m_CpuSamples.size() >= TimedFrameCount)
								m_Phase = Phase::Drain;
							break;
						}

						case Phase::Drain:
							if (!m_DiagnosticsCaptureStopped)
							{
								App.StopDiagnosticsCapture();
								m_DiagnosticsCaptureStopped = true;
							}
							RequirePreparedScene();
							++m_DrainFrames;
							if (m_DrainFrames >= MaximumDrainFrameCount || GpuTimingCollectionComplete(App))
								m_Phase = Phase::RenderOutput;
							break;

						case Phase::RenderOutput:
							RequirePreparedScene();
							break;

						case Phase::ReadOutput:
							ReadOutputAndReport(App);
							break;

						case Phase::Complete:
							break;
					}
				}
				catch (const std::exception& Exception)
				{
					Fail(Exception.what());
				}
			}

			void OnRender() override
			{
				if (!m_Succeeded || m_Phase == Phase::Complete) return;
				if (m_Phase == Phase::RenderOutput)
				{
					const auto Rendered = m_Renderer->RenderPreparedSceneToHdr(OutputWidth, OutputHeight);
					if (!Rendered)
					{
						Fail(Rendered.error().Message);
						return;
					}
					m_OutputTexture = Rendered->ColorTexture;
					m_LastDrawCount = Rendered->GeometryDrawCount;
					m_LastPreparedCount = m_Renderer->GetPreparedMeshCount();
					m_Phase = Phase::ReadOutput;
					return;
				}

				const auto Rendered = m_Renderer->RenderPreparedScene();
				if (!Rendered)
				{
					Fail(Rendered.error().Message);
					return;
				}
				m_LastDrawCount = *Rendered;
				m_LastPreparedCount = m_Renderer->GetPreparedMeshCount();
			}

			[[nodiscard]] bool Succeeded() const noexcept { return m_Succeeded; }

		private:
			void FindSourceMeshAndCreateWorkload()
			{
				for (const Entity& Candidate : m_Scene.GetEntities())
				{
					const auto Tag = Candidate.GetTag();
					const auto Mesh = Candidate.GetMeshRenderer();
					if (!Tag || !Mesh || !*Mesh || Tag->Name != "Rough clay - high roughness") continue;
					m_SourceMesh = **Mesh;
					m_SourceName = Tag->Name;
					break;
				}
				if (m_SourceName.empty())
					throw std::runtime_error("PBR scene has no non-floor mesh instance to replicate");

				for (size_t Index = 0; Index < m_AdditionalInstances; ++Index)
				{
					auto Created = m_Scene.CreateEntityWithUUID(
						UUID(GeneratedEntityUUIDHigh, static_cast<uint64_t>(Index + 1)),
						"G27 generated instance " + std::to_string(Index));
					if (!Created) throw std::runtime_error("Generated entity creation failed: " + Created.error().Message);
					if (const auto Assigned = Created->SetMeshRenderer(m_SourceMesh); !Assigned)
						throw std::runtime_error("Generated mesh assignment failed: " + Assigned.error().Message);
					const size_t Column = Index % 16;
					const size_t Row = Index / 16;
					TransformComponent Transform;
					Transform.Translation = glm::vec3(
						-6.0f + static_cast<float>(Column) * 0.8f,
						0.35f,
						-4.8f + static_cast<float>(Row) * 0.8f);
					Transform.Scale = glm::vec3(0.28f);
					if (const auto Set = Created->SetTransform(Transform); !Set)
						throw std::runtime_error("Generated transform assignment failed: " + Set.error().Message);
				}
			}

			[[nodiscard]] std::expected<void, SceneRendererError> PrepareScene()
			{
				return m_Renderer->PrepareScene(m_Scene, m_SceneAspectRatio);
			}

			void RequirePreparedScene()
			{
				if (const auto Result = PrepareScene(); !Result) throw std::runtime_error(Result.error().Message);
			}

			void CollectGpuTimings(Application& App)
			{
				if (m_GpuSessionID == 0) return;
				App.CollectCompletedGpuFrameTimings();
				std::array<GpuFrameTiming, 64> Timings{};
				const size_t Count = App.ReadCompletedGpuFrameTimings(Timings);
				for (size_t Index = 0; Index < Count; ++Index)
					if (Timings[Index].SessionId == m_GpuSessionID)
					{
						if (Timings[Index].DurationNanoseconds == 0)
							++m_ZeroGpuSamples;
						else
							m_GpuSamples.push_back(Timings[Index].DurationNanoseconds);
					}
			}

			[[nodiscard]] bool GpuTimingCollectionComplete(Application& App) const
			{
				if (!App.IsGpuFrameTimingSupported() || App.HasGpuFrameTimingFailed()) return true;
				const GpuFrameTimingCollectionState State = App.GetGpuFrameTimingCollectionState(m_GpuSessionID);
				return State.RecordingCount == 0 && State.PendingCount == 0 && !State.Failed;
			}

			void ReadOutputAndReport(Application& App)
			{
				if (!m_OutputTexture) throw std::runtime_error("Untimed HDR frame did not return a color texture");
				const auto Readback = App.ReadTextureForValidation(*m_OutputTexture);
				if (!Readback) throw std::runtime_error("HDR readback failed: " + Readback.error().Message);
				m_OutputHash = HashReadback(*Readback);
				m_ReadbackWidth = Readback->Width;
				m_ReadbackHeight = Readback->Height;
				m_ReadbackFormat = static_cast<uint32_t>(Readback->Format);
				m_OutputTexture = nullptr;
				if (m_GpuSamples.empty() && m_ZeroGpuSamples > 0)
					m_GpuTimingUnavailableReason = "GPU timer queries returned only zero-duration samples";
				else if (m_GpuSamples.size() + m_ZeroGpuSamples != m_CpuSamples.size())
					m_GpuTimingUnavailableReason = "GPU timer query coverage did not match the measured CPU frame count";
				else if (m_ZeroGpuSamples > 0)
					m_GpuTimingUnavailableReason = "GPU timer queries included zero-duration samples";
				else if (m_GpuSamples.size() != m_CpuSamples.size())
					m_GpuTimingUnavailableReason = "GPU timer query sample count did not match the measured CPU frame count";
				WriteReport(App);
				m_Phase = Phase::Complete;
				App.RequestClose();
			}

			void WriteReport(const Application& App)
			{
				std::ostringstream Stream;
				Stream.precision(6);
				Stream << "{\"schema\":2,\"fixture\":\"PBRMaterialsOverview-start-scene\""
					   << ",\"project\":\"" << EscapeJson(std::filesystem::path(PF_PBR_DEMO_PROJECT_FILE).filename().string()) << '\"'
					   << ",\"scene_asset_id\":\"" << m_SceneAssetID.ToString() << '\"'
					   << ",\"replica_source\":\"" << EscapeJson(m_SourceName) << '\"'
					   << ",\"mesh_asset_id\":\"" << m_SourceMesh.MeshAsset.ToString() << '\"'
					   << ",\"material_asset_id\":\""
					   << (m_SourceMesh.MaterialAsset ? m_SourceMesh.MaterialAsset->ToString() : std::string("none")) << '\"'
					   << ",\"workload\":\"" << (m_AdditionalInstances == 0 ? "small-reference" : "generated-many") << '\"'
					   << ",\"additional_instances\":" << m_AdditionalInstances
					   << ",\"generated_seed\":0"
					   << ",\"prepared_mesh_count\":" << m_LastPreparedCount
					   << ",\"geometry_draw_count\":" << m_LastDrawCount
					   << ",\"window_resolution\":{\"width\":" << m_Width << ",\"height\":" << m_Height << '}'
					   << ",\"scene_aspect_ratio\":" << m_SceneAspectRatio
					   << ",\"readback_resolution\":{\"width\":" << m_ReadbackWidth << ",\"height\":" << m_ReadbackHeight << '}'
					   << ",\"renderer\":\"Vulkan\",\"hardware\":\"" << EscapeJson(App.GetRendererHardwareName()) << '\"'
					   << ",\"cpu_prepare\":" << PercentilesJson(m_CpuSamples)
					   << ",\"cpu_snapshot_build\":" << PercentilesJson(m_SnapshotBuildSamples)
					   << ",\"cpu_renderer_prepare\":" << PercentilesJson(m_RendererPrepareSamples)
					   << ",\"gpu_frame_work\":" << PercentilesJson(m_GpuSamples)
					   << ",\"gpu_timing_sample_count\":" << (m_GpuSamples.size() + m_ZeroGpuSamples)
					   << ",\"gpu_timing_expected_sample_count\":" << m_CpuSamples.size()
					   << ",\"gpu_timing_zero_duration_count\":" << m_ZeroGpuSamples
					   << ",\"gpu_timing_coverage_complete\":"
					   << ((m_GpuSamples.size() == m_CpuSamples.size() && m_ZeroGpuSamples == 0) ? "true" : "false")
					   << ",\"gpu_timing_supported\":" << (App.IsGpuFrameTimingSupported() ? "true" : "false")
					   << ",\"gpu_timing_failed\":" << (App.HasGpuFrameTimingFailed() ? "true" : "false")
					   << ",\"gpu_timing_unavailable_reason\":\""
					   << EscapeJson(m_GpuTimingUnavailableReason.empty()
							? std::string(App.GetGpuFrameTimingUnavailableReason()) : m_GpuTimingUnavailableReason) << '\"'
					   << ",\"readback_format\":" << m_ReadbackFormat
					   << ",\"output_hash_fnv1a64\":\"0x" << std::hex << m_OutputHash << std::dec << '\"'
					   << ",\"presentation_latency\":\"unavailable\"}";
				const std::string Json = Stream.str();
				std::cout << "G27_BASELINE=" << Json << '\n';
				if (m_OutputPath.empty()) return;
				std::ofstream Output(m_OutputPath, std::ios::binary | std::ios::trunc);
				if (!Output) throw std::runtime_error("Could not open optional output path: " + m_OutputPath.string());
				Output << Json << '\n';
				if (!Output) throw std::runtime_error("Could not write optional output path: " + m_OutputPath.string());
			}

			void Fail(std::string_view Reason)
			{
				m_Succeeded = false;
				Application::Get().StopDiagnosticsCapture();
				PF_CORE_ERROR("G27 renderer scaling baseline failed: {0}", Reason);
				Application::Get().RequestClose();
			}

			const size_t m_AdditionalInstances;
			const std::filesystem::path m_OutputPath;
			std::optional<Project> m_Project;
			Scene m_Scene;
			std::unique_ptr<SceneRenderer> m_Renderer;
			MeshRendererComponent m_SourceMesh{};
			std::string m_SourceName;
			AssetID m_SceneAssetID{};
			const Texture* m_OutputTexture = nullptr;
			std::vector<uint64_t> m_CpuSamples;
			std::vector<uint64_t> m_SnapshotBuildSamples;
			std::vector<uint64_t> m_RendererPrepareSamples;
			std::vector<uint64_t> m_GpuSamples;
			std::string m_GpuTimingUnavailableReason;
			Phase m_Phase = Phase::Warmup;
			uint64_t m_GpuSessionID = 0;
			uint64_t m_OutputHash = 0;
			uint32_t m_Width = OutputWidth;
			uint32_t m_Height = OutputHeight;
			const float m_SceneAspectRatio = static_cast<float>(OutputWidth) / static_cast<float>(OutputHeight);
			uint32_t m_ReadbackWidth = 0;
			uint32_t m_ReadbackHeight = 0;
			uint32_t m_ReadbackFormat = 0;
			std::chrono::steady_clock::time_point m_WarmupStartedAt{};
			size_t m_ReadyWarmupFrames = 0;
			uint32_t m_DrainFrames = 0;
			size_t m_LastDrawCount = 0;
			size_t m_LastPreparedCount = 0;
			size_t m_ZeroGpuSamples = 0;
			bool m_Succeeded = true;
			bool m_DiagnosticsCaptureStopped = false;
		};

		class ScalingApplication final : public Application
		{
		public:
			ScalingApplication(size_t AdditionalInstances, std::filesystem::path OutputPath)
				: Application(RendererAPI::Vulkan)
			{
				auto Layer = std::make_unique<ScalingLayer>(AdditionalInstances, std::move(OutputPath));
				m_Layer = Layer.get();
				PushLayer(std::move(Layer));
			}

			int RunValidation()
			{
				Run();
				return m_Layer && m_Layer->Succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
			}

		private:
			ScalingLayer* m_Layer = nullptr;
		};
	} // namespace

	// This standalone validation executable receives its workload count in argv[1] and an optional output path in argv[2].
	int RunRendererScalingBaselineValidation(int ArgumentCount, char** Arguments)
	{
		size_t AdditionalInstances = DefaultAdditionalInstanceCount;
		std::filesystem::path OutputPath;
		if (ArgumentCount > 1)
		{
			const std::string_view Text(Arguments[1]);
			const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), AdditionalInstances);
			if (Error != std::errc{} || End != Text.data() + Text.size())
				throw std::runtime_error("Generated instance count must be a nonnegative integer");
		}
		if (ArgumentCount > 2) OutputPath = std::filesystem::path(Arguments[2]);
		if (AdditionalInstances > 4096)
			throw std::runtime_error("Generated workload is capped at 4096 additional mesh instances");
		ScalingApplication App(AdditionalInstances, std::move(OutputPath));
		return App.RunValidation();
	}
} // namespace PulseForge::GpuValidation
