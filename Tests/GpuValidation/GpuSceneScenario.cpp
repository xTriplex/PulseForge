#include "GpuValidation/GpuSceneScenario.h"

#include <stdexcept>

namespace PulseForge::GpuValidation
{
	void BuildGpuValidationSceneScenario(
		const Scene& SourceScene, Scene& Destination, bool IncludeGeometry, bool IncludeEnvironment, bool IncludeDirectional)
	{
		bool HasCamera = false;
		for (const Entity Source : SourceScene.GetEntities())
		{
			const auto Transform = Source.GetTransform();
			const auto Camera = Source.GetCamera();
			if (!Transform || !Camera) throw std::runtime_error("Could not inspect bundled scene entities for GPU validation scenarios");
			if (!HasCamera && *Camera)
			{
				auto Entity = Destination.CreateEntity("GPU validation camera");
				if (!Entity) throw std::runtime_error("Could not create scenario camera: " + Entity.error().Message);
				CameraComponent Component = **Camera;
				Component.IsPrimary = true;
				if (auto Result = Entity->SetTransform(*Transform); !Result)
					throw std::runtime_error("Could not set scenario camera transform: " + Result.error().Message);
				if (auto Result = Entity->SetCamera(Component); !Result)
					throw std::runtime_error("Could not set scenario camera: " + Result.error().Message);
				HasCamera = true;
			}

			const auto Environment = Source.GetEnvironmentLight();
			const auto Directional = Source.GetDirectionalLight();
			const auto Mesh = Source.GetMeshRenderer();
			if (!Environment || !Directional || !Mesh)
				throw std::runtime_error("Could not inspect bundled scene components for GPU validation scenarios");
			if ((IncludeEnvironment && *Environment) || (IncludeDirectional && *Directional) || (IncludeGeometry && *Mesh))
			{
				auto Entity = Destination.CreateEntity("GPU validation scenario object");
				if (!Entity) throw std::runtime_error("Could not create scenario object: " + Entity.error().Message);
				if (auto Result = Entity->SetTransform(*Transform); !Result)
					throw std::runtime_error("Could not set scenario object transform: " + Result.error().Message);
				if (IncludeEnvironment && *Environment)
					if (auto Result = Entity->SetEnvironmentLight(**Environment); !Result)
						throw std::runtime_error("Could not set scenario environment: " + Result.error().Message);
				if (IncludeDirectional && *Directional)
					if (auto Result = Entity->SetDirectionalLight(**Directional); !Result)
						throw std::runtime_error("Could not set scenario directional light: " + Result.error().Message);
				if (IncludeGeometry && *Mesh)
					if (auto Result = Entity->SetMeshRenderer(**Mesh); !Result)
						throw std::runtime_error("Could not set scenario mesh: " + Result.error().Message);
			}
		}
		if (!HasCamera) throw std::runtime_error("Bundled validation scene has no camera for GPU scenario setup");
	}
} // namespace PulseForge::GpuValidation
