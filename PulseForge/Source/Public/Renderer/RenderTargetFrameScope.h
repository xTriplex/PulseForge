#pragma once

#include "Core/Log.h"
#include "Renderer/RenderDevice.h"

#include <exception>

namespace PulseForge
{
	class RenderTargetFrameScope final
	{
	public:
		explicit RenderTargetFrameScope(RenderDevice& Runtime) : m_Runtime(Runtime) {}

		RenderTargetFrameScope(const RenderTargetFrameScope&) = delete;
		RenderTargetFrameScope& operator=(const RenderTargetFrameScope&) = delete;

		~RenderTargetFrameScope() noexcept
		{
			if (!m_Active) return;

			try
			{
				const GraphicsResult EndResult = m_Runtime.EndRenderTarget();
				if (!EndResult)
					PF_CORE_ERROR("Could not close render target during scene-renderer unwinding: {0}", EndResult.error().Message);
			}
			catch (const std::exception& Exception)
			{
				PF_CORE_ERROR("Exception while closing render target during scene-renderer unwinding: {0}", Exception.what());
			}
			catch (...)
			{
				PF_CORE_ERROR("Unknown exception while closing render target during scene-renderer unwinding");
			}
		}

		GraphicsResult End()
		{
			m_Active = false;
			return m_Runtime.EndRenderTarget();
		}

	private:
		RenderDevice& m_Runtime;
		bool m_Active = true;
	};
} // namespace PulseForge
