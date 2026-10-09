#include "Core/PulseForgePCH.h"

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include "Renderer/RendererBackend.h"
#include "Renderer/Vulkan/VulkanSupport.h"
#include "Window/Window.h"

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <GLFW/glfw3.h>

#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace PulseForge
{
	namespace
	{
		constexpr uint32_t VulkanApiVersion = VK_API_VERSION_1_3;
		constexpr size_t FramesInFlight = 2;
		constexpr std::array<float, 4> ClearColor = { 0.025f, 0.08f, 0.16f, 1.0f };

		std::string ApiVersionString(uint32_t Version)
		{
			return std::to_string(VK_API_VERSION_MAJOR(Version)) + "." +
				std::to_string(VK_API_VERSION_MINOR(Version)) + "." +
				std::to_string(VK_API_VERSION_PATCH(Version));
		}

		void CheckVkResult(VkResult Result, const char* Operation)
		{
			if (Result != VK_SUCCESS)
			{
				throw std::runtime_error(std::string(Operation) + " failed with Vulkan result " +
					vk::to_string(static_cast<vk::Result>(Result)));
			}
		}

		bool HasName(const std::vector<vk::ExtensionProperties>& Properties, const char* Name)
		{
			return std::any_of(Properties.begin(), Properties.end(), [Name](const auto& Property)
			{
				return std::strcmp(Property.extensionName, Name) == 0;
			});
		}

		bool HasName(const std::vector<vk::LayerProperties>& Properties, const char* Name)
		{
			return std::any_of(Properties.begin(), Properties.end(), [Name](const auto& Property)
			{
				return std::strcmp(Property.layerName, Name) == 0;
			});
		}

		bool HasName(const std::vector<const char*>& Names, const char* Name)
		{
			return std::any_of(Names.begin(), Names.end(), [Name](const char* Existing)
			{
				return std::strcmp(Existing, Name) == 0;
			});
		}

		std::optional<nvrhi::Format> ToNvrhiFormat(vk::Format Format)
		{
			switch (Format)
			{
				case vk::Format::eB8G8R8A8Srgb: return nvrhi::Format::SBGRA8_UNORM;
				case vk::Format::eB8G8R8A8Unorm: return nvrhi::Format::BGRA8_UNORM;
				case vk::Format::eR8G8B8A8Srgb: return nvrhi::Format::SRGBA8_UNORM;
				case vk::Format::eR8G8B8A8Unorm: return nvrhi::Format::RGBA8_UNORM;
				default: return std::nullopt;
			}
		}

		std::optional<std::pair<vk::SurfaceFormatKHR, nvrhi::Format>> ChooseSupportedSurfaceFormat(
			const std::vector<vk::SurfaceFormatKHR>& Formats)
		{
			if (Formats.empty())
				return std::nullopt;

			std::vector<VulkanSupport::SurfaceFormat> CandidateFormats;
			CandidateFormats.reserve(Formats.size());
			for (const auto& Format : Formats)
			{
				CandidateFormats.push_back({ static_cast<uint32_t>(Format.format), static_cast<uint32_t>(Format.colorSpace) });
			}

			constexpr uint32_t SrgbNonlinear = static_cast<uint32_t>(vk::ColorSpaceKHR::eSrgbNonlinear);
			constexpr uint32_t UndefinedFormat = static_cast<uint32_t>(vk::Format::eUndefined);
			constexpr std::array PreferredFormats = {
				vk::Format::eB8G8R8A8Srgb,
				vk::Format::eB8G8R8A8Unorm,
				vk::Format::eR8G8B8A8Srgb,
				vk::Format::eR8G8B8A8Unorm
			};

			for (vk::Format Preferred : PreferredFormats)
			{
				auto PreferredResult = VulkanSupport::ChooseSurfaceFormat(
					CandidateFormats,
					static_cast<uint32_t>(Preferred),
					SrgbNonlinear,
					UndefinedFormat);
				if (!PreferredResult)
					continue;

				const auto Format = static_cast<vk::Format>(PreferredResult->Format);
				auto NvrhiFormat = ToNvrhiFormat(Format);
				if (!NvrhiFormat)
					continue;

				return std::pair{
					vk::SurfaceFormatKHR(Format, static_cast<vk::ColorSpaceKHR>(PreferredResult->ColorSpace)),
					*NvrhiFormat
				};
			}

			for (const auto& Format : Formats)
			{
				auto NvrhiFormat = ToNvrhiFormat(Format.format);
				if (NvrhiFormat)
					return std::pair{ Format, *NvrhiFormat };
			}

			return std::nullopt;
		}

		VulkanSupport::PresentMode ToSupportPresentMode(vk::PresentModeKHR Mode)
		{
			switch (Mode)
			{
				case vk::PresentModeKHR::eImmediate: return VulkanSupport::PresentMode::Immediate;
				case vk::PresentModeKHR::eMailbox: return VulkanSupport::PresentMode::Mailbox;
				case vk::PresentModeKHR::eFifo: return VulkanSupport::PresentMode::Fifo;
				default: return VulkanSupport::PresentMode::Other;
			}
		}

		vk::PresentModeKHR ToVkPresentMode(VulkanSupport::PresentMode Mode)
		{
			switch (Mode)
			{
				case VulkanSupport::PresentMode::Immediate: return vk::PresentModeKHR::eImmediate;
				case VulkanSupport::PresentMode::Mailbox: return vk::PresentModeKHR::eMailbox;
				case VulkanSupport::PresentMode::Fifo: return vk::PresentModeKHR::eFifo;
				case VulkanSupport::PresentMode::Other: break;
			}
			return vk::PresentModeKHR::eFifo;
		}

		template<typename THandle>
		uint64_t ToNativeHandleValue(THandle Handle)
		{
			if constexpr (std::is_pointer_v<THandle>)
				return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Handle));
			else
				return static_cast<uint64_t>(Handle);
		}

		VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
			VkDebugUtilsMessageSeverityFlagBitsEXT Severity,
			VkDebugUtilsMessageTypeFlagsEXT MessageType,
			const VkDebugUtilsMessengerCallbackDataEXT* CallbackData,
			void*)
		{
			const char* Message = CallbackData && CallbackData->pMessage
				? CallbackData->pMessage
				: "Vulkan validation callback provided no message";
			const char* MessageId = CallbackData && CallbackData->pMessageIdName
				? CallbackData->pMessageIdName
				: "unknown";

			const uint32_t MessageTypeValue = static_cast<uint32_t>(MessageType);
			switch (Severity)
			{
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
					PF_CORE_ERROR("Vulkan error (type={0:#x}, id={1}): {2}", MessageTypeValue, MessageId, Message);
					break;
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
					PF_CORE_WARN("Vulkan warning (type={0:#x}, id={1}): {2}", MessageTypeValue, MessageId, Message);
					break;
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
					PF_CORE_INFO("Vulkan info (type={0:#x}, id={1}): {2}", MessageTypeValue, MessageId, Message);
					break;
				case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
					PF_CORE_TRACE("Vulkan verbose (type={0:#x}, id={1}): {2}", MessageTypeValue, MessageId, Message);
					break;
				default:
					PF_CORE_WARN("Vulkan message with unknown severity {0:#x} (type={1:#x}, id={2}): {3}",
						static_cast<uint32_t>(Severity), MessageTypeValue, MessageId, Message);
					break;
			}

			return VK_FALSE;
		}

		class NvrhiMessageCallback final : public nvrhi::IMessageCallback
		{
		public:
			void message(nvrhi::MessageSeverity Severity, const char* Text) override
			{
				const char* SafeText = Text ? Text : "NVRHI reported an empty diagnostic";
				switch (Severity)
				{
					case nvrhi::MessageSeverity::Info:
						PF_CORE_INFO("NVRHI: {0}", SafeText);
						break;
					case nvrhi::MessageSeverity::Warning:
						PF_CORE_WARN("NVRHI: {0}", SafeText);
						break;
					case nvrhi::MessageSeverity::Error:
					case nvrhi::MessageSeverity::Fatal:
						PF_CORE_ERROR("NVRHI: {0}", SafeText);
						break;
				}
			}
		};

		class VulkanBuffer final : public Buffer
		{
		public:
			VulkanBuffer(BufferDesc Description, nvrhi::BufferHandle Handle)
				: m_Description(std::move(Description)),
				  m_Handle(std::move(Handle))
			{
			}

			const BufferDesc& GetDescription() const noexcept override
			{
				return m_Description;
			}

			nvrhi::IBuffer* GetNativeBuffer() const noexcept
			{
				return m_Handle.Get();
			}

		private:
			BufferDesc m_Description;
			nvrhi::BufferHandle m_Handle;
		};

		class VulkanTexture final : public Texture
		{
		public:
			VulkanTexture(TextureDesc Description, nvrhi::TextureHandle Handle)
				: m_Description(std::move(Description)),
				  m_Handle(std::move(Handle))
			{
			}

			const TextureDesc& GetDescription() const noexcept override
			{
				return m_Description;
			}

			nvrhi::ITexture* GetNativeTexture() const noexcept
			{
				return m_Handle.Get();
			}

			nvrhi::TextureHandle GetNativeTextureHandle() const noexcept
			{
				return m_Handle;
			}

		private:
			TextureDesc m_Description;
			nvrhi::TextureHandle m_Handle;
		};

		class VulkanRenderTarget final : public RenderTarget
		{
		public:
			VulkanRenderTarget(
				RenderTargetDesc Description,
				TextureHandle ColorTexture,
				TextureHandle DepthTexture,
				nvrhi::FramebufferHandle Framebuffer)
				: m_Description(std::move(Description)),
				  m_ColorTexture(std::move(ColorTexture)),
				  m_DepthTexture(std::move(DepthTexture)),
				  m_Framebuffer(std::move(Framebuffer))
			{
			}

			const RenderTargetDesc& GetDescription() const noexcept override
			{
				return m_Description;
			}

			const Texture* GetColorTexture() const noexcept override
			{
				return m_ColorTexture.get();
			}

			const Texture* GetDepthTexture() const noexcept override
			{
				return m_DepthTexture.get();
			}

			nvrhi::FramebufferHandle GetNativeFramebuffer() const noexcept
			{
				return m_Framebuffer;
			}

			nvrhi::TextureHandle GetNativeColorTexture() const noexcept
			{
				return m_ColorTexture
					? static_cast<const VulkanTexture&>(*m_ColorTexture).GetNativeTextureHandle()
					: nvrhi::TextureHandle{};
			}

			nvrhi::TextureHandle GetNativeDepthTexture() const noexcept
			{
				return m_DepthTexture
					? static_cast<const VulkanTexture&>(*m_DepthTexture).GetNativeTextureHandle()
					: nvrhi::TextureHandle{};
			}

		private:
			// Declaration order ensures the framebuffer releases its attachment references first.
			RenderTargetDesc m_Description;
			TextureHandle m_ColorTexture;
			TextureHandle m_DepthTexture;
			nvrhi::FramebufferHandle m_Framebuffer;
		};

		class VulkanSampler final : public Sampler
		{
		public:
			VulkanSampler(SamplerDesc Description, nvrhi::SamplerHandle Handle)
				: m_Description(std::move(Description)),
				  m_Handle(std::move(Handle))
			{
			}

			const SamplerDesc& GetDescription() const noexcept override
			{
				return m_Description;
			}

			nvrhi::ISampler* GetNativeSampler() const noexcept
			{
				return m_Handle.Get();
			}

		private:
			SamplerDesc m_Description;
			nvrhi::SamplerHandle m_Handle;
		};

		class VulkanShader final : public Shader
		{
		public:
			VulkanShader(ShaderDesc Description, std::vector<std::byte> Bytecode, nvrhi::ShaderHandle Handle)
				: m_Description(std::move(Description)),
				  m_Bytecode(std::move(Bytecode)),
				  m_Handle(std::move(Handle))
			{
			}

			const ShaderDesc& GetDescription() const noexcept override
			{
				return m_Description;
			}

			nvrhi::IShader* GetNativeShader() const noexcept
			{
				return m_Handle.Get();
			}

		private:
			ShaderDesc m_Description;
			std::vector<std::byte> m_Bytecode;
			nvrhi::ShaderHandle m_Handle;
		};

		class VulkanBindingLayout final : public BindingLayout
		{
		public:
			VulkanBindingLayout(BindingLayoutDesc Description, nvrhi::BindingLayoutHandle Handle)
				: m_Description(std::move(Description)),
				  m_Handle(std::move(Handle))
			{
			}

			const BindingLayoutDesc& GetDescription() const noexcept override
			{
				return m_Description;
			}

			nvrhi::IBindingLayout* GetNativeLayout() const noexcept
			{
				return m_Handle.Get();
			}

		private:
			BindingLayoutDesc m_Description;
			nvrhi::BindingLayoutHandle m_Handle;
		};

		class VulkanBindingSet final : public BindingSet
		{
		public:
			VulkanBindingSet(BindingLayoutHandle Layout, nvrhi::BindingSetHandle Handle)
				: m_Layout(std::move(Layout)),
				  m_Handle(std::move(Handle))
			{
			}

			const BindingLayout& GetLayout() const noexcept override
			{
				return *m_Layout;
			}

			nvrhi::IBindingSet* GetNativeBindingSet() const noexcept
			{
				return m_Handle.Get();
			}

		private:
			// NVRHI retains the native resources referenced by the set; this retains its engine layout.
			BindingLayoutHandle m_Layout;
			nvrhi::BindingSetHandle m_Handle;
		};

		class VulkanGraphicsPipeline final : public GraphicsPipeline
		{
		public:
			VulkanGraphicsPipeline(
				GraphicsPipelineDesc Description,
				nvrhi::InputLayoutHandle InputLayout,
				nvrhi::GraphicsPipelineHandle Handle)
				: m_Description(std::move(Description)),
				  m_InputLayout(std::move(InputLayout)),
				  m_Handle(std::move(Handle))
			{
			}

			const GraphicsPipelineDesc& GetDescription() const noexcept override
			{
				return m_Description;
			}

			nvrhi::IGraphicsPipeline* GetNativePipeline() const noexcept
			{
				return m_Handle.Get();
			}

		private:
			// Reverse member destruction releases the pipeline before the input layout and shaders.
			GraphicsPipelineDesc m_Description;
			nvrhi::InputLayoutHandle m_InputLayout;
			nvrhi::GraphicsPipelineHandle m_Handle;
		};

		struct DeviceCandidate
		{
			vk::PhysicalDevice Device;
			vk::PhysicalDeviceProperties Properties;
			VulkanSupport::QueueFamilySelection QueueFamilies;
			vk::SurfaceFormatKHR SurfaceFormat;
			nvrhi::Format NvrhiFormat = nvrhi::Format::UNKNOWN;
			vk::PresentModeKHR PresentMode = vk::PresentModeKHR::eFifo;
			int Score = 0;
		};

		bool HasRequiredSurfaceUsage(const vk::SurfaceCapabilitiesKHR& Capabilities)
		{
			constexpr VkImageUsageFlags RequiredUsage =
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			return (static_cast<VkImageUsageFlags>(Capabilities.supportedUsageFlags) & RequiredUsage) == RequiredUsage;
		}

		bool SupportsDepth32Attachment(vk::PhysicalDevice Device)
		{
			const vk::FormatProperties Properties = Device.getFormatProperties(vk::Format::eD32Sfloat);
			return (static_cast<VkFormatFeatureFlags>(Properties.optimalTilingFeatures) &
				VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
		}

		vk::CompositeAlphaFlagBitsKHR ChooseCompositeAlpha(vk::CompositeAlphaFlagsKHR Supported)
		{
			constexpr std::array Preferred = {
				vk::CompositeAlphaFlagBitsKHR::eOpaque,
				vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
				vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
				vk::CompositeAlphaFlagBitsKHR::eInherit
			};
			const VkCompositeAlphaFlagsKHR SupportedBits = static_cast<VkCompositeAlphaFlagsKHR>(Supported);
			for (const auto Alpha : Preferred)
			{
				if ((SupportedBits & static_cast<VkCompositeAlphaFlagsKHR>(Alpha)) != 0)
					return Alpha;
			}
			throw std::runtime_error("Vulkan surface reports no supported composite-alpha mode");
		}
	}

	class VulkanRenderer final : public RendererBackend
	{
	public:
		explicit VulkanRenderer(Window& Window)
			: m_Window(Window),
			  m_NativeWindow(static_cast<GLFWwindow*>(Window.GetNativeWindow()))
		{
			try
			{
				Initialize();
			}
			catch (const std::exception& Exception)
			{
				PF_CORE_ERROR("Vulkan renderer initialization failed: {0}", Exception.what());
				Shutdown();
				throw;
			}
		}

		~VulkanRenderer() override
		{
			Shutdown();
		}

		OutputColorEncoding GetOutputColorEncoding() const noexcept override
		{
			switch (m_SurfaceFormat.format)
			{
				case vk::Format::eB8G8R8A8Srgb:
				case vk::Format::eR8G8B8A8Srgb:
					return OutputColorEncoding::SrgbAttachment;
				default:
					return OutputColorEncoding::UnormAttachment;
			}
		}

		bool BeginFrame() override
		{
			if (!m_Device)
				throw std::runtime_error("Vulkan renderer is not initialized");

			const auto [FramebufferWidth, FramebufferHeight] = m_Window.GetFramebufferSize();
			if (FramebufferWidth == 0 || FramebufferHeight == 0)
				return false;

			if (!m_Swapchain || m_SwapchainRecreationNeeded ||
				FramebufferWidth != m_SwapchainExtent.width || FramebufferHeight != m_SwapchainExtent.height)
			{
				if (!RecreateSwapchain())
					return false;
			}

			FrameContext& Frame = m_Frames[m_CurrentFrame];
			WaitForFrame(Frame);

			uint32_t ImageIndex = 0;
			const VkResult AcquireResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(
				static_cast<VkDevice>(m_Device),
				static_cast<VkSwapchainKHR>(m_Swapchain),
				std::numeric_limits<uint64_t>::max(),
				static_cast<VkSemaphore>(Frame.ImageAvailable),
				VK_NULL_HANDLE,
				&ImageIndex);

			if (AcquireResult == VK_ERROR_OUT_OF_DATE_KHR)
			{
				m_SwapchainRecreationNeeded = true;
				(void)RecreateSwapchain();
				return false;
			}
			if (AcquireResult != VK_SUCCESS && AcquireResult != VK_SUBOPTIMAL_KHR)
				CheckVkResult(AcquireResult, "vkAcquireNextImageKHR");

			if (ImageIndex >= m_SwapchainImages.size())
				throw std::runtime_error("Vulkan returned a swapchain image index outside the image array");

			m_AcquiredImageIndex = ImageIndex;
			m_AcquiredSuboptimal = AcquireResult == VK_SUBOPTIMAL_KHR;
			SwapchainImage& Image = m_SwapchainImages[m_AcquiredImageIndex];
			const auto* DepthTarget = dynamic_cast<const VulkanTexture*>(Image.DepthTarget.get());
			if (!DepthTarget)
				throw std::runtime_error("Vulkan swapchain image is missing its PulseForge depth target");

			Frame.CommandList->open();
			m_ActiveRenderTarget.reset();
			Frame.CommandList->clearTextureFloat(
				Image.Texture,
				nvrhi::TextureSubresourceSet(),
				nvrhi::Color(ClearColor[0], ClearColor[1], ClearColor[2], ClearColor[3]));
			Frame.CommandList->clearDepthStencilTexture(
				DepthTarget->GetNativeTexture(),
				nvrhi::TextureSubresourceSet(),
				true,
				1.0f,
				false,
				0);
			m_FrameActive = true;
			return true;
		}

		void EndFrame() override
		{
			if (!m_FrameActive)
				return;

			if (m_ActiveRenderTarget)
			{
				PF_CORE_WARN("EndFrame closed an offscreen render target that was left active");
				const GraphicsResult TargetEndResult = EndRenderTarget();
				if (!TargetEndResult)
					PF_CORE_ERROR("Could not close active offscreen render target: {0}", TargetEndResult.error().Message);
			}

			FrameContext& Frame = m_Frames[m_CurrentFrame];
			SwapchainImage& Image = m_SwapchainImages[m_AcquiredImageIndex];

			m_VulkanDevice->queueWaitForSemaphore(
				nvrhi::CommandQueue::Graphics,
				static_cast<VkSemaphore>(Frame.ImageAvailable),
				0);
			m_VulkanDevice->queueSignalSemaphore(
				nvrhi::CommandQueue::Graphics,
				static_cast<VkSemaphore>(Image.PresentReady),
				0);

			Frame.CommandList->setTextureState(
				Image.Texture,
				nvrhi::TextureSubresourceSet(),
				nvrhi::ResourceStates::Present);
			Frame.CommandList->commitBarriers();
			Frame.CommandList->close();
			Frame.SubmissionValue = m_ActiveNvrhiDevice->executeCommandList(
				Frame.CommandList,
				nvrhi::CommandQueue::Graphics);

			const VkSemaphore PresentSemaphore = static_cast<VkSemaphore>(Image.PresentReady);
			const VkSwapchainKHR Swapchain = static_cast<VkSwapchainKHR>(m_Swapchain);
			const VkPresentInfoKHR PresentInfo{
				VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
				nullptr,
				1,
				&PresentSemaphore,
				1,
				&Swapchain,
				&m_AcquiredImageIndex,
				nullptr
			};
			const VkResult PresentResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(
				static_cast<VkQueue>(m_PresentQueue),
				&PresentInfo);
			m_ActiveNvrhiDevice->runGarbageCollection();

			const bool AcquiredSuboptimal = m_AcquiredSuboptimal;
			m_FrameActive = false;
			m_AcquiredSuboptimal = false;
			m_CurrentFrame = (m_CurrentFrame + 1) % m_Frames.size();

			if (PresentResult != VK_SUCCESS &&
				PresentResult != VK_SUBOPTIMAL_KHR &&
				PresentResult != VK_ERROR_OUT_OF_DATE_KHR)
			{
				CheckVkResult(PresentResult, "vkQueuePresentKHR");
			}

			if (PresentResult == VK_SUCCESS && !m_LoggedFirstPresentedFrameForSwapchain)
			{
				m_LoggedFirstPresentedFrameForSwapchain = true;
				PF_CORE_INFO(
					"First Vulkan/NVRHI frame submitted and presented successfully (geometry draw recorded: {0})",
					m_LoggedFirstDraw);
			}

			if (AcquiredSuboptimal || PresentResult == VK_ERROR_OUT_OF_DATE_KHR || PresentResult == VK_SUBOPTIMAL_KHR)
			{
				m_SwapchainRecreationNeeded = true;
				return;
			}
		}

		GraphicsResult BeginRenderTarget(
			const RenderTarget& Target,
			const RenderTargetClearValue& ClearValue) override
		{
			if (!m_FrameActive)
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::InvalidFrameState,
					"Offscreen render targets may only begin during an active renderer frame"
				});
			}
			if (m_ActiveRenderTarget)
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::InvalidFrameState,
					"PulseForge does not support nested render targets"
				});
			}

			const auto ClearValidation = ValidateRenderTargetClearValue(ClearValue);
			if (!ClearValidation)
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::InvalidDescription,
					ClearValidation.error().Message
				});
			}

			const auto* NativeTarget = dynamic_cast<const VulkanRenderTarget*>(&Target);
			if (!NativeTarget)
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::UnsupportedFeature,
					"Vulkan rendering requires a render target created by the active Vulkan renderer"
				});
			}

			try
			{
				nvrhi::TextureHandle ColorTexture = NativeTarget->GetNativeColorTexture();
				nvrhi::TextureHandle DepthTexture = NativeTarget->GetNativeDepthTexture();
				FrameContext& Frame = m_Frames[m_CurrentFrame];
				if (ColorTexture)
					Frame.CommandList->setTextureState(
						ColorTexture,
						nvrhi::TextureSubresourceSet(),
						nvrhi::ResourceStates::RenderTarget);
				if (DepthTexture)
					Frame.CommandList->setTextureState(
						DepthTexture,
						nvrhi::TextureSubresourceSet(),
						nvrhi::ResourceStates::DepthWrite);
				Frame.CommandList->commitBarriers();
				if (ColorTexture)
				{
					Frame.CommandList->clearTextureFloat(
						ColorTexture,
						nvrhi::TextureSubresourceSet(),
						nvrhi::Color(ClearValue.Color[0], ClearValue.Color[1], ClearValue.Color[2], ClearValue.Color[3]));
				}
				if (DepthTexture)
					Frame.CommandList->clearDepthStencilTexture(
						DepthTexture,
						nvrhi::TextureSubresourceSet(),
						true,
						ClearValue.Depth,
						false,
						0);

				m_ActiveRenderTarget = ActiveRenderTarget{
					NativeTarget->GetDescription().ColorFormat,
					NativeTarget->GetDescription().Width,
					NativeTarget->GetDescription().Height,
					NativeTarget->GetDescription().DepthMode == DepthAttachmentMode::ShaderReadableAttachment,
					NativeTarget->GetDescription().DepthMode != DepthAttachmentMode::None,
					NativeTarget->GetNativeFramebuffer(),
					std::move(ColorTexture),
					std::move(DepthTexture)
				};
				return {};
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI render-target begin failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		GraphicsResult EndRenderTarget() override
		{
			if (!m_FrameActive || !m_ActiveRenderTarget)
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::InvalidFrameState,
					"EndRenderTarget requires an active frame and an active offscreen render target"
				});
			}

			try
			{
				FrameContext& Frame = m_Frames[m_CurrentFrame];
				if (m_ActiveRenderTarget->ColorTexture)
					Frame.CommandList->setTextureState(
						m_ActiveRenderTarget->ColorTexture,
						nvrhi::TextureSubresourceSet(),
						nvrhi::ResourceStates::ShaderResource);
				if (m_ActiveRenderTarget->DepthShaderResource && m_ActiveRenderTarget->DepthTexture)
					Frame.CommandList->setTextureState(
						m_ActiveRenderTarget->DepthTexture,
						nvrhi::TextureSubresourceSet(),
						nvrhi::ResourceStates::ShaderResource);
				Frame.CommandList->commitBarriers();
				m_ActiveRenderTarget.reset();
				return {};
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI render-target end failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		BufferCreateResult CreateBuffer(
			const BufferDesc& Description,
			std::span<const std::byte> InitialData) override
		{
			nvrhi::ResourceStates InitialState = nvrhi::ResourceStates::VertexBuffer;
			nvrhi::BufferDesc NvrhiDescription;
			NvrhiDescription
				.setByteSize(Description.ByteSize)
				.setIsVertexBuffer(Description.Usage == BufferUsage::Vertex)
				.setIsIndexBuffer(Description.Usage == BufferUsage::Index)
				.setIsConstantBuffer(Description.Usage == BufferUsage::Constant)
				.setDebugName(Description.DebugName.empty() ? "PulseForge buffer" : Description.DebugName);
			if (Description.Usage == BufferUsage::Index)
				InitialState = nvrhi::ResourceStates::IndexBuffer;
			else if (Description.Usage == BufferUsage::Constant)
				InitialState = nvrhi::ResourceStates::ConstantBuffer;
			NvrhiDescription.enableAutomaticStateTracking(InitialState);

			try
			{
				nvrhi::BufferHandle NativeBuffer = m_ActiveNvrhiDevice->createBuffer(NvrhiDescription);
				if (!NativeBuffer)
				{
					const std::string Message = "NVRHI failed to create a renderer buffer";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(BufferCreateError{
						BufferCreateErrorCode::BackendFailure,
						Message
					});
				}

				if (!InitialData.empty())
				{
					nvrhi::CommandListHandle UploadCommandList = m_ActiveNvrhiDevice->createCommandList();
					if (!UploadCommandList)
					{
						const std::string Message = "NVRHI failed to create a buffer upload command list";
						PF_CORE_ERROR("{0}", Message);
						return std::unexpected(BufferCreateError{
							BufferCreateErrorCode::BackendFailure,
							Message
						});
					}

					UploadCommandList->open();
					UploadCommandList->writeBuffer(NativeBuffer, InitialData.data(), InitialData.size());
					UploadCommandList->close();
					m_ActiveNvrhiDevice->executeCommandList(UploadCommandList, nvrhi::CommandQueue::Graphics);
				}

				BufferHandle EngineBuffer = std::make_unique<VulkanBuffer>(Description, std::move(NativeBuffer));
				return BufferCreateResult(std::move(EngineBuffer));
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = std::string("NVRHI buffer creation/upload failed: ") + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(BufferCreateError{
					BufferCreateErrorCode::BackendFailure,
					Message
				});
			}
		}

		BufferUpdateResult WriteBuffer(
			const Buffer& Target,
			uint64_t DestinationOffset,
			std::span<const std::byte> Data) override
		{
			if (!m_FrameActive)
			{
				return std::unexpected(BufferUpdateError{
					BufferUpdateErrorCode::InvalidFrameState,
					"Vulkan buffer updates must be recorded between BeginFrame and EndFrame"
				});
			}

			const auto* NativeBuffer = dynamic_cast<const VulkanBuffer*>(&Target);
			if (!NativeBuffer)
			{
				return std::unexpected(BufferUpdateError{
					BufferUpdateErrorCode::BackendFailure,
					"The buffer was not created by the active Vulkan renderer"
				});
			}

			try
			{
				m_Frames[m_CurrentFrame].CommandList->writeBuffer(
					NativeBuffer->GetNativeBuffer(),
					Data.data(),
					Data.size(),
					DestinationOffset);
				return {};
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = std::string("NVRHI failed to record a buffer update: ") + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(BufferUpdateError{
					BufferUpdateErrorCode::BackendFailure,
					Message
				});
			}
		}

		BindingLayoutCreateResult CreateBindingLayout(const BindingLayoutDesc& Description) override
		{
			nvrhi::ShaderType Visibility = nvrhi::ShaderType::Pixel;
			switch (Description.Visibility)
			{
				case ShaderVisibility::Vertex:
					Visibility = nvrhi::ShaderType::Vertex;
					break;
				case ShaderVisibility::Fragment:
					Visibility = nvrhi::ShaderType::Pixel;
					break;
				case ShaderVisibility::AllGraphics:
					Visibility = nvrhi::ShaderType::AllGraphics;
					break;
			}

			nvrhi::BindingLayoutDesc NativeDescription;
			NativeDescription
				.setVisibility(Visibility)
				.setRegisterSpaceAndDescriptorSet(Description.ShaderRegisterSpace)
				.setBindingOffsets(nvrhi::VulkanBindingOffsets()
					.setShaderResourceOffset(0)
					.setSamplerOffset(128)
					.setConstantBufferOffset(256));

			for (const BindingLayoutItemDesc& Item : Description.Items)
			{
				switch (Item.Type)
				{
					case BindingResourceType::Texture2D:
					case BindingResourceType::TextureCube:
						NativeDescription.addItem(nvrhi::BindingLayoutItem::Texture_SRV(Item.Slot));
						break;
					case BindingResourceType::Sampler:
						NativeDescription.addItem(nvrhi::BindingLayoutItem::Sampler(Item.Slot));
						break;
					case BindingResourceType::ConstantBuffer:
						NativeDescription.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(Item.Slot));
						break;
				}
			}

			try
			{
				nvrhi::BindingLayoutHandle NativeLayout = m_ActiveNvrhiDevice->createBindingLayout(NativeDescription);
				if (!NativeLayout)
				{
					const std::string Message = "NVRHI failed to create binding layout '" + Description.DebugName + "'";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(BindingError{ BindingErrorCode::BackendFailure, Message });
				}

				PF_CORE_INFO("Created Vulkan/NVRHI binding layout '{0}'", Description.DebugName);
				return BindingLayoutHandle(std::make_shared<VulkanBindingLayout>(Description, std::move(NativeLayout)));
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI binding layout creation failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(BindingError{ BindingErrorCode::BackendFailure, Message });
			}
		}

		TextureCreateResult CreateTexture(
			const TextureDesc& Description,
			std::span<const TextureSubresourceData> InitialData) override
		{
			const auto Validation = ValidateTextureUpload(Description, InitialData);
			if (!Validation)
				return std::unexpected(Validation.error());

			nvrhi::Format Format = nvrhi::Format::UNKNOWN;
			switch (Description.Format)
			{
				case TextureFormat::RGBA8_UNorm: Format = nvrhi::Format::RGBA8_UNORM; break;
				case TextureFormat::RGBA8_Srgb: Format = nvrhi::Format::SRGBA8_UNORM; break;
				case TextureFormat::RGBA32_Float: Format = nvrhi::Format::RGBA32_FLOAT; break;
				case TextureFormat::Depth32Float: Format = nvrhi::Format::D32; break;
			}
			const bool IsDepthAttachment = Description.Format == TextureFormat::Depth32Float;
			const bool IsColorAttachment = HasTextureUsage(Description.Usage, TextureUsage::ColorAttachment);
			const bool IsShaderResource = HasTextureUsage(Description.Usage, TextureUsage::ShaderResource);
			const nvrhi::ResourceStates InitialState = IsDepthAttachment
				? nvrhi::ResourceStates::DepthWrite
				: IsColorAttachment
					? nvrhi::ResourceStates::RenderTarget
					: nvrhi::ResourceStates::ShaderResource;

			nvrhi::TextureDesc NativeDescription;
			NativeDescription
				.setDimension(Description.Dimension == TextureDimension::TextureCube
					? nvrhi::TextureDimension::TextureCube
					: nvrhi::TextureDimension::Texture2D)
				.setWidth(Description.Width)
				.setHeight(Description.Height)
				.setArraySize(Description.Dimension == TextureDimension::TextureCube ? 6u : 1u)
				.setMipLevels(Description.MipLevels)
				.setFormat(Format)
				.setIsRenderTarget(IsDepthAttachment || IsColorAttachment)
				.enableAutomaticStateTracking(InitialState)
				.setDebugName(Description.DebugName.empty() ? "PulseForge texture" : Description.DebugName);
			NativeDescription.isShaderResource = IsShaderResource;

			try
			{
				nvrhi::TextureHandle NativeTexture = m_ActiveNvrhiDevice->createTexture(NativeDescription);
				if (!NativeTexture)
				{
					const std::string Message = "NVRHI failed to create texture '" + Description.DebugName + "'";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
				}

				if (IsDepthAttachment)
				{
					PF_CORE_INFO("Created Vulkan/NVRHI {0}x{1} D32 depth attachment '{2}'",
						Description.Width,
						Description.Height,
						Description.DebugName);
					return TextureHandle(std::make_unique<VulkanTexture>(Description, std::move(NativeTexture)));
				}

				if (InitialData.empty())
					return TextureHandle(std::make_unique<VulkanTexture>(Description, std::move(NativeTexture)));

				nvrhi::CommandListHandle UploadCommandList = m_ActiveNvrhiDevice->createCommandList();
				if (!UploadCommandList)
				{
					const std::string Message = "NVRHI failed to create a texture upload command list";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
				}

				UploadCommandList->open();
				UploadCommandList->setTextureState(NativeTexture, nvrhi::TextureSubresourceSet(), nvrhi::ResourceStates::CopyDest);
				UploadCommandList->commitBarriers();
				for (const TextureSubresourceData& Subresource : InitialData)
				{
					const size_t BytesPerPixel = Description.Format == TextureFormat::RGBA32_Float ? 16 : 4;
					const size_t RowPitch = Subresource.RowPitch == 0
						? static_cast<size_t>(std::max(1u, Description.Width >> Subresource.MipLevel)) * BytesPerPixel
						: Subresource.RowPitch;
					UploadCommandList->writeTexture(
						NativeTexture,
						Subresource.ArraySlice,
						Subresource.MipLevel,
						Subresource.Data.data(),
						RowPitch,
						0);
				}
				UploadCommandList->setTextureState(
					NativeTexture,
					nvrhi::TextureSubresourceSet(),
					IsShaderResource ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::RenderTarget);
				UploadCommandList->commitBarriers();
				UploadCommandList->close();
				m_ActiveNvrhiDevice->executeCommandList(UploadCommandList, nvrhi::CommandQueue::Graphics);

				PF_CORE_INFO("Uploaded Vulkan/NVRHI {0}x{1} texture '{2}'",
					Description.Width,
					Description.Height,
					Description.DebugName);
				return TextureHandle(std::make_unique<VulkanTexture>(Description, std::move(NativeTexture)));
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI texture creation/upload failed for '" +
					Description.DebugName + "': " + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
			}
		}

		RenderTargetCreateResult CreateRenderTarget(const RenderTargetDesc& Description) override
		{
			const auto Validation = ValidateRenderTargetDescription(Description);
			if (!Validation)
				return std::unexpected(Validation.error());

			TextureCreateResult ColorTexture = TextureHandle{};
			if (Description.ColorFormat != ColorTargetFormat::None)
			{
				TextureDesc ColorDescription;
				ColorDescription.Width = Description.Width;
				ColorDescription.Height = Description.Height;
				ColorDescription.Format = Description.ColorFormat == ColorTargetFormat::RGBA8_UNorm
					? TextureFormat::RGBA8_UNorm
					: TextureFormat::RGBA8_Srgb;
				ColorDescription.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
				ColorDescription.DebugName = Description.DebugName.empty()
					? "PulseForge offscreen color"
					: Description.DebugName + " color";
				ColorTexture = CreateTexture(ColorDescription, {});
				if (!ColorTexture)
					return std::unexpected(RenderTargetError{ RenderTargetErrorCode::BackendFailure,
						"Could not create render-target color texture: " + ColorTexture.error().Message });
			}

			TextureCreateResult DepthTexture = TextureHandle{};
			if (Description.DepthMode != DepthAttachmentMode::None)
			{
				TextureDesc DepthDescription;
				DepthDescription.Width = Description.Width;
				DepthDescription.Height = Description.Height;
				DepthDescription.Format = TextureFormat::Depth32Float;
				DepthDescription.Usage = Description.DepthMode == DepthAttachmentMode::ShaderReadableAttachment
					? TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource
					: TextureUsage::DepthStencilAttachment;
				DepthDescription.DebugName = Description.DebugName.empty()
					? "PulseForge offscreen depth"
					: Description.DebugName + " depth";
				DepthTexture = CreateTexture(DepthDescription, {});
				if (!DepthTexture)
				{
					return std::unexpected(RenderTargetError{
						RenderTargetErrorCode::BackendFailure,
						"Could not create render-target depth texture: " + DepthTexture.error().Message
					});
				}
			}

			const bool HasColorTexture = Description.ColorFormat != ColorTargetFormat::None;
			const auto* NativeColor = HasColorTexture ? dynamic_cast<const VulkanTexture*>(ColorTexture->get()) : nullptr;
			const auto* NativeDepth = DepthTexture ? dynamic_cast<const VulkanTexture*>(DepthTexture->get()) : nullptr;
			if ((HasColorTexture && !NativeColor) ||
				(Description.DepthMode != DepthAttachmentMode::None && !NativeDepth))
			{
				return std::unexpected(RenderTargetError{
					RenderTargetErrorCode::UnsupportedFeature,
					"Vulkan render targets require textures created by the active Vulkan renderer"
				});
			}

			try
			{
				nvrhi::FramebufferDesc NativeDescription;
				if (NativeColor)
					NativeDescription.addColorAttachment(NativeColor->GetNativeTextureHandle());
				if (NativeDepth)
					NativeDescription.setDepthAttachment(NativeDepth->GetNativeTexture());
				nvrhi::FramebufferHandle Framebuffer = m_ActiveNvrhiDevice->createFramebuffer(NativeDescription);
				if (!Framebuffer)
				{
					return std::unexpected(RenderTargetError{
						RenderTargetErrorCode::BackendFailure,
						"NVRHI could not create the offscreen render-target framebuffer"
					});
				}

				PF_CORE_INFO(
					"Created Vulkan/NVRHI {0}x{1} offscreen render target '{2}'",
					Description.Width,
					Description.Height,
					Description.DebugName);
				return RenderTargetHandle(std::make_unique<VulkanRenderTarget>(
					Description,
					HasColorTexture ? std::move(ColorTexture.value()) : TextureHandle{},
					DepthTexture ? std::move(DepthTexture.value()) : TextureHandle{},
					std::move(Framebuffer)));
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI render-target creation failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(RenderTargetError{ RenderTargetErrorCode::BackendFailure, Message });
			}
		}

		SamplerCreateResult CreateSampler(const SamplerDesc& Description) override
		{
			const auto ToNativeAddressMode = [](SamplerAddressMode Mode)
			{
				return Mode == SamplerAddressMode::Repeat
					? nvrhi::SamplerAddressMode::Wrap
					: nvrhi::SamplerAddressMode::Clamp;
			};
			nvrhi::SamplerDesc NativeDescription;
			NativeDescription
				.setMinFilter(Description.Minification == SamplerFilter::Linear)
				.setMagFilter(Description.Magnification == SamplerFilter::Linear)
				.setMipFilter(Description.Mip != SamplerMipFilter::None)
				.setAddressU(ToNativeAddressMode(Description.AddressU))
				.setAddressV(ToNativeAddressMode(Description.AddressV))
				.setAddressW(nvrhi::SamplerAddressMode::Clamp);

			try
			{
				nvrhi::SamplerHandle NativeSampler = m_ActiveNvrhiDevice->createSampler(NativeDescription);
				if (!NativeSampler)
				{
					const std::string Message = "NVRHI failed to create sampler '" + Description.DebugName + "'";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
				}

				PF_CORE_INFO("Created Vulkan/NVRHI sampler '{0}'", Description.DebugName);
				return SamplerHandle(std::make_unique<VulkanSampler>(Description, std::move(NativeSampler)));
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI sampler creation failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
			}
		}

		BindingSetCreateResult CreateBindingSet(const BindingSetDesc& Description) override
		{
			auto Layout = std::dynamic_pointer_cast<VulkanBindingLayout>(Description.Layout);
			if (!Layout)
			{
				return std::unexpected(BindingError{
					BindingErrorCode::UnsupportedFeature,
					"Vulkan binding sets require a layout created by the active Vulkan renderer"
				});
			}

			nvrhi::BindingSetDesc NativeDescription;
			for (const TextureBindingDesc& Item : Description.Textures)
			{
				const auto* Texture = dynamic_cast<const VulkanTexture*>(&Item.Resource.get());
				if (!Texture)
				{
					return std::unexpected(BindingError{
						BindingErrorCode::UnsupportedFeature,
						"Vulkan texture bindings require textures created by the active Vulkan renderer"
					});
				}
				NativeDescription.addItem(nvrhi::BindingSetItem::Texture_SRV(
					Item.Slot,
					Texture->GetNativeTexture(),
					nvrhi::Format::UNKNOWN,
					nvrhi::AllSubresources,
					Item.Type == BindingResourceType::TextureCube
						? nvrhi::TextureDimension::TextureCube
						: nvrhi::TextureDimension::Texture2D));
			}

			for (const SamplerBindingDesc& Item : Description.Samplers)
			{
				const auto* Sampler = dynamic_cast<const VulkanSampler*>(&Item.Resource.get());
				if (!Sampler)
				{
					return std::unexpected(BindingError{
						BindingErrorCode::UnsupportedFeature,
						"Vulkan sampler bindings require samplers created by the active Vulkan renderer"
					});
				}
				NativeDescription.addItem(nvrhi::BindingSetItem::Sampler(
					Item.Slot,
					Sampler->GetNativeSampler()));
			}

			for (const BufferBindingDesc& Item : Description.Buffers)
			{
				const auto* Buffer = dynamic_cast<const VulkanBuffer*>(&Item.Resource.get());
				if (!Buffer)
				{
					return std::unexpected(BindingError{
						BindingErrorCode::UnsupportedFeature,
						"Vulkan constant-buffer bindings require buffers created by the active Vulkan renderer"
					});
				}
				NativeDescription.addItem(nvrhi::BindingSetItem::ConstantBuffer(
					Item.Slot,
					Buffer->GetNativeBuffer()));
			}

			try
			{
				nvrhi::BindingSetHandle NativeSet = m_ActiveNvrhiDevice->createBindingSet(
					NativeDescription,
					Layout->GetNativeLayout());
				if (!NativeSet)
				{
					const std::string Message = "NVRHI failed to create a binding set for '" +
						Description.Layout->GetDescription().DebugName + "'";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(BindingError{ BindingErrorCode::BackendFailure, Message });
				}

				return BindingSetHandle(std::make_unique<VulkanBindingSet>(Description.Layout, std::move(NativeSet)));
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI binding-set creation failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(BindingError{ BindingErrorCode::BackendFailure, Message });
			}
		}

		ShaderCreateResult CreateShader(
			const ShaderDesc& Description,
			std::span<const std::byte> Bytecode) override
		{
			std::vector<std::byte> OwnedBytecode(Bytecode.begin(), Bytecode.end());
			nvrhi::ShaderDesc NativeDescription;
			NativeDescription
				.setShaderType(Description.Stage == ShaderStage::Vertex
					? nvrhi::ShaderType::Vertex
					: nvrhi::ShaderType::Pixel)
				.setEntryName(Description.EntryPoint)
				.setDebugName(Description.DebugName.empty() ? "PulseForge shader" : Description.DebugName);

			try
			{
				nvrhi::ShaderHandle NativeShader = m_ActiveNvrhiDevice->createShader(
					NativeDescription,
					OwnedBytecode.data(),
					OwnedBytecode.size());
				if (!NativeShader)
				{
					const std::string Message = "NVRHI rejected SPIR-V shader '" + Description.DebugName +
						"' at entry point '" + Description.EntryPoint + "'";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
				}

				const char* StageName = Description.Stage == ShaderStage::Vertex ? "vertex" : "fragment";
				PF_CORE_INFO("Created Vulkan/NVRHI {0} shader '{1}'", StageName, Description.DebugName);
				return ShaderHandle(std::make_shared<VulkanShader>(
					Description,
					std::move(OwnedBytecode),
					std::move(NativeShader)));
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI shader creation failed for '" + Description.DebugName + "': " + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		GraphicsPipelineCreateResult CreateGraphicsPipeline(const GraphicsPipelineDesc& Description) override
		{
			const auto VertexShader = std::dynamic_pointer_cast<VulkanShader>(Description.VertexShader);
			const auto FragmentShader = std::dynamic_pointer_cast<VulkanShader>(Description.FragmentShader);
		if (!VertexShader || (Description.FragmentShader && !FragmentShader))
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::UnsupportedFeature,
					"Vulkan graphics pipelines require shader objects created by the active Vulkan renderer"
				});
			}

			std::vector<std::shared_ptr<VulkanBindingLayout>> NativeBindingLayouts;
			NativeBindingLayouts.reserve(Description.BindingLayouts.size());
			for (const BindingLayoutHandle& LayoutHandle : Description.BindingLayouts)
			{
				auto Layout = std::dynamic_pointer_cast<VulkanBindingLayout>(LayoutHandle);
				if (!Layout)
				{
					return std::unexpected(GraphicsError{
						GraphicsErrorCode::UnsupportedFeature,
						"Vulkan graphics pipelines require binding layouts created by the active Vulkan renderer"
					});
				}
				NativeBindingLayouts.push_back(std::move(Layout));
			}

			std::vector<VertexAttributeDesc> OrderedAttributes = Description.VertexLayout.Attributes;
			// NVRHI's Vulkan input layout assigns locations by array order; semantic order is the shader-location contract.
			std::sort(OrderedAttributes.begin(), OrderedAttributes.end(), [](
				const VertexAttributeDesc& Left,
				const VertexAttributeDesc& Right)
			{
				return Left.Semantic < Right.Semantic;
			});

			std::vector<nvrhi::VertexAttributeDesc> NativeAttributes;
			NativeAttributes.reserve(OrderedAttributes.size());
			for (const VertexAttributeDesc& Attribute : OrderedAttributes)
			{
				const char* SemanticName = "POSITION";
				switch (Attribute.Semantic)
				{
					case VertexSemantic::Position: SemanticName = "POSITION"; break;
					case VertexSemantic::Color: SemanticName = "COLOR"; break;
					case VertexSemantic::TexCoord: SemanticName = "TEXCOORD"; break;
					case VertexSemantic::Normal: SemanticName = "NORMAL"; break;
				}
				nvrhi::Format NativeFormat = nvrhi::Format::UNKNOWN;
				switch (Attribute.Format)
				{
					case VertexFormat::Float2: NativeFormat = nvrhi::Format::RG32_FLOAT; break;
					case VertexFormat::Float3: NativeFormat = nvrhi::Format::RGB32_FLOAT; break;
					case VertexFormat::Float4: NativeFormat = nvrhi::Format::RGBA32_FLOAT; break;
				}

				nvrhi::VertexAttributeDesc NativeAttribute;
				NativeAttribute
					.setName(SemanticName)
					.setFormat(NativeFormat)
					.setOffset(Attribute.Offset)
					.setElementStride(Description.VertexLayout.Stride);
				NativeAttributes.push_back(std::move(NativeAttribute));
			}

			try
			{
				nvrhi::InputLayoutHandle InputLayout = m_ActiveNvrhiDevice->createInputLayout(
					NativeAttributes.data(),
					static_cast<uint32_t>(NativeAttributes.size()),
					VertexShader->GetNativeShader());
				if (!InputLayout)
				{
					const std::string Message = "NVRHI failed to create the vertex input layout for pipeline '" + Description.DebugName + "'";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
				}

				nvrhi::RasterState Rasterizer;
				switch (Description.Rasterizer.Cull)
				{
					case CullMode::None: Rasterizer.setCullNone(); break;
					case CullMode::Back: Rasterizer.setCullBack(); break;
					case CullMode::Front: Rasterizer.setCullFront(); break;
				}
				Rasterizer
					.setFillMode(Description.Rasterizer.Wireframe
						? nvrhi::RasterFillMode::Wireframe
						: nvrhi::RasterFillMode::Solid)
					.setScissorEnable(Description.Rasterizer.ScissorEnabled)
					.setDepthBias(static_cast<int32_t>(std::lround(Description.Rasterizer.DepthBias)))
					.setSlopeScaleDepthBias(Description.Rasterizer.SlopeScaledDepthBias);

				nvrhi::ComparisonFunc DepthComparison = nvrhi::ComparisonFunc::Less;
				switch (Description.Depth.Compare)
				{
					case DepthCompareOperation::Never: DepthComparison = nvrhi::ComparisonFunc::Never; break;
					case DepthCompareOperation::Less: DepthComparison = nvrhi::ComparisonFunc::Less; break;
					case DepthCompareOperation::Equal: DepthComparison = nvrhi::ComparisonFunc::Equal; break;
					case DepthCompareOperation::LessEqual: DepthComparison = nvrhi::ComparisonFunc::LessOrEqual; break;
					case DepthCompareOperation::Greater: DepthComparison = nvrhi::ComparisonFunc::Greater; break;
					case DepthCompareOperation::NotEqual: DepthComparison = nvrhi::ComparisonFunc::NotEqual; break;
					case DepthCompareOperation::GreaterEqual: DepthComparison = nvrhi::ComparisonFunc::GreaterOrEqual; break;
					case DepthCompareOperation::Always: DepthComparison = nvrhi::ComparisonFunc::Always; break;
				}
				nvrhi::DepthStencilState DepthState;
				DepthState
					.setDepthTestEnable(Description.Depth.TestEnabled)
					.setDepthWriteEnable(Description.Depth.WriteEnabled)
					.setDepthFunc(DepthComparison);

				nvrhi::BlendState BlendState;
				if (Description.Blend.Enabled)
				{
					nvrhi::BlendState::RenderTarget BlendTarget;
					BlendTarget
						.enableBlend()
						.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
						.setDestBlend(nvrhi::BlendFactor::OneMinusSrcAlpha)
						.setSrcBlendAlpha(nvrhi::BlendFactor::One)
						.setDestBlendAlpha(nvrhi::BlendFactor::OneMinusSrcAlpha);
					BlendState.setRenderTarget(0, BlendTarget);
				}

				nvrhi::RenderState RenderState;
				RenderState
					.setRasterState(Rasterizer)
					.setDepthStencilState(DepthState)
					.setBlendState(BlendState);

				nvrhi::GraphicsPipelineDesc NativeDescription;
				NativeDescription
					.setPrimType(nvrhi::PrimitiveType::TriangleList)
					.setInputLayout(InputLayout.Get())
					.setVertexShader(VertexShader->GetNativeShader())
					.setRenderState(RenderState);
				if (FragmentShader)
					NativeDescription.setPixelShader(FragmentShader->GetNativeShader());
				for (const auto& Layout : NativeBindingLayouts)
					NativeDescription.addBindingLayout(Layout->GetNativeLayout());

				nvrhi::FramebufferInfo FramebufferInfo;
				if (Description.ColorFormat != ColorTargetFormat::None)
					FramebufferInfo.addColorFormat(Description.ColorFormat == ColorTargetFormat::Swapchain
						? m_NvrhiFormat
						: Description.ColorFormat == ColorTargetFormat::RGBA8_UNorm
							? nvrhi::Format::RGBA8_UNORM
							: nvrhi::Format::SRGBA8_UNORM);
				if (Description.DepthAttachmentEnabled)
					FramebufferInfo.setDepthFormat(nvrhi::Format::D32);
				nvrhi::GraphicsPipelineHandle NativePipeline = m_ActiveNvrhiDevice->createGraphicsPipeline(
					NativeDescription,
					FramebufferInfo);
				if (!NativePipeline)
				{
					const std::string Message = "NVRHI failed to create graphics pipeline '" + Description.DebugName + "'";
					PF_CORE_ERROR("{0}", Message);
					return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
				}

				PF_CORE_INFO("Created Vulkan/NVRHI graphics pipeline '{0}'", Description.DebugName);
				GraphicsPipelineHandle Pipeline = std::make_unique<VulkanGraphicsPipeline>(
					Description,
					std::move(InputLayout),
					std::move(NativePipeline));
				return Pipeline;
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = "Vulkan/NVRHI pipeline creation failed for '" + Description.DebugName + "': " + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		GraphicsResult Draw(
			const GraphicsPipeline& Pipeline,
			const Buffer& VertexBuffer,
			const DrawArguments& Arguments,
			std::span<const BindingSet* const> BindingSets) override
		{
			bool SkipDraw = false;
			const GraphicsResult StateResult = SetDrawState(
				Pipeline,
				VertexBuffer,
				nullptr,
				BindingSets,
				std::nullopt,
				SkipDraw);
			if (!StateResult)
				return StateResult;
			if (SkipDraw)
				return {};

			try
			{
				FrameContext& Frame = m_Frames[m_CurrentFrame];
				nvrhi::DrawArguments NativeArguments;
				NativeArguments
					.setVertexCount(Arguments.VertexCount)
					.setInstanceCount(Arguments.InstanceCount)
					.setStartVertexLocation(Arguments.FirstVertex)
					.setStartInstanceLocation(Arguments.FirstInstance);
				Frame.CommandList->draw(NativeArguments);
				if (!m_LoggedFirstDraw)
				{
					m_LoggedFirstDraw = true;
					PF_CORE_INFO("Bound PulseForge vertex buffer and recorded the first NVRHI geometry draw");
				}
				return {};
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = std::string("Vulkan/NVRHI draw recording failed: ") + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		GraphicsResult DrawIndexed(
			const GraphicsPipeline& Pipeline,
			const Buffer& VertexBuffer,
			const Buffer& IndexBuffer,
			const DrawIndexedArguments& Arguments,
			std::span<const BindingSet* const> BindingSets) override
		{
			bool SkipDraw = false;
			const GraphicsResult StateResult = SetDrawState(
				Pipeline,
				VertexBuffer,
				&IndexBuffer,
				BindingSets,
				Arguments.Scissor,
				SkipDraw);
			if (!StateResult)
				return StateResult;
			if (SkipDraw)
				return {};

			try
			{
				FrameContext& Frame = m_Frames[m_CurrentFrame];
				nvrhi::DrawArguments NativeArguments;
				NativeArguments
					.setVertexCount(Arguments.IndexCount)
					.setInstanceCount(Arguments.InstanceCount)
					.setStartIndexLocation(Arguments.FirstIndex)
					.setStartVertexLocation(0)
					.setStartInstanceLocation(Arguments.FirstInstance);
				Frame.CommandList->drawIndexed(NativeArguments);
				if (!m_LoggedFirstDraw)
				{
					m_LoggedFirstDraw = true;
					PF_CORE_INFO("Bound PulseForge vertex/index buffers and recorded the first NVRHI indexed geometry draw");
				}
				return {};
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = std::string("Vulkan/NVRHI indexed draw recording failed: ") + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

	private:
		GraphicsResult SetDrawState(
			const GraphicsPipeline& Pipeline,
			const Buffer& VertexBuffer,
			const Buffer* IndexBuffer,
			std::span<const BindingSet* const> BindingSets,
			const std::optional<ScissorRect>& Scissor,
			bool& OutSkipDraw)
		{
			OutSkipDraw = false;
			if (!m_FrameActive)
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::InvalidFrameState,
					"PulseForge draw commands may only be submitted during an active renderer frame"
				});
			}

			const auto* NativePipeline = dynamic_cast<const VulkanGraphicsPipeline*>(&Pipeline);
			const auto* NativeVertexBuffer = dynamic_cast<const VulkanBuffer*>(&VertexBuffer);
			const auto* NativeIndexBuffer = IndexBuffer ? dynamic_cast<const VulkanBuffer*>(IndexBuffer) : nullptr;
			if (!NativePipeline || !NativeVertexBuffer ||
				(IndexBuffer && (!NativeIndexBuffer || IndexBuffer->GetDescription().Usage != BufferUsage::Index)))
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::UnsupportedFeature,
					"Vulkan draws require compatible resources created by the active Vulkan renderer"
				});
			}
			if (VertexBuffer.GetDescription().Usage != BufferUsage::Vertex)
				return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDrawArguments, "Vulkan draw requires a vertex buffer" });

			try
			{
				nvrhi::FramebufferHandle Framebuffer;
				uint32_t TargetWidth = 0;
				uint32_t TargetHeight = 0;
				ColorTargetFormat TargetFormat = ColorTargetFormat::Swapchain;
				if (m_ActiveRenderTarget)
				{
					TargetFormat = m_ActiveRenderTarget->ColorFormat;
					TargetWidth = m_ActiveRenderTarget->Width;
					TargetHeight = m_ActiveRenderTarget->Height;
					Framebuffer = m_ActiveRenderTarget->Framebuffer;
				}
				else
				{
					SwapchainImage& Image = m_SwapchainImages[m_AcquiredImageIndex];
					TargetWidth = m_SwapchainExtent.width;
					TargetHeight = m_SwapchainExtent.height;
					Framebuffer = Image.Framebuffer;
				}

				const bool TargetHasDepth = m_ActiveRenderTarget
					? m_ActiveRenderTarget->HasDepthAttachment
					: true;
				if (NativePipeline->GetDescription().ColorFormat != TargetFormat ||
					NativePipeline->GetDescription().DepthAttachmentEnabled != TargetHasDepth)
				{
					return std::unexpected(GraphicsError{
						GraphicsErrorCode::InvalidDescription,
						"Graphics pipeline color format does not match the active render target"
					});
				}

				const nvrhi::Viewport Viewport(
					static_cast<float>(TargetWidth),
					static_cast<float>(TargetHeight));
				nvrhi::ViewportState ViewportState;
				ViewportState.addViewport(Viewport);
				if (Scissor)
				{
					// Draw data may reflect a just-resized window before the active swapchain is recreated.
					const std::optional<ScissorRect> ClippedScissor = IntersectScissorRect(
						*Scissor,
						TargetWidth,
						TargetHeight);
					if (!ClippedScissor)
					{
						OutSkipDraw = true;
						return {};
					}
					const uint64_t Right = static_cast<uint64_t>(ClippedScissor->X) + ClippedScissor->Width;
					const uint64_t Bottom = static_cast<uint64_t>(ClippedScissor->Y) + ClippedScissor->Height;
					if (Right > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
						Bottom > static_cast<uint64_t>(std::numeric_limits<int>::max()))
					{
						return std::unexpected(GraphicsError{
							GraphicsErrorCode::InvalidDrawArguments,
							"Scissor exceeds NVRHI integer limits"
						});
					}
					ViewportState.addScissorRect(nvrhi::Rect(
						static_cast<int>(ClippedScissor->X),
						static_cast<int>(Right),
						static_cast<int>(ClippedScissor->Y),
						static_cast<int>(Bottom)));
				}
				else
					ViewportState.addScissorRect(nvrhi::Rect(Viewport));

				nvrhi::GraphicsState State;
				State
					.setPipeline(NativePipeline->GetNativePipeline())
					.setFramebuffer(Framebuffer)
					.setViewport(ViewportState)
					.addVertexBuffer(nvrhi::VertexBufferBinding()
						.setBuffer(NativeVertexBuffer->GetNativeBuffer())
						.setSlot(0)
						.setOffset(0));
				if (NativeIndexBuffer)
				{
					State.setIndexBuffer(nvrhi::IndexBufferBinding()
						.setBuffer(NativeIndexBuffer->GetNativeBuffer())
						.setFormat(nvrhi::Format::R32_UINT)
						.setOffset(0));
				}

				for (const BindingSet* Binding : BindingSets)
				{
					const auto* NativeBindingSet = dynamic_cast<const VulkanBindingSet*>(Binding);
					if (!NativeBindingSet)
					{
						return std::unexpected(GraphicsError{
							GraphicsErrorCode::UnsupportedFeature,
							"Vulkan draws require binding sets created by the active Vulkan renderer"
						});
					}
					State.addBindingSet(NativeBindingSet->GetNativeBindingSet());
				}

				m_Frames[m_CurrentFrame].CommandList->setGraphicsState(State);
				return {};
			}
			catch (const std::exception& Exception)
			{
				const std::string Message = std::string("Vulkan/NVRHI graphics state binding failed: ") + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		struct FrameContext
		{
			vk::Semaphore ImageAvailable;
			nvrhi::CommandListHandle CommandList;
			uint64_t SubmissionValue = 0;
		};

		struct ActiveRenderTarget
		{
			ColorTargetFormat ColorFormat = ColorTargetFormat::Swapchain;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool DepthShaderResource = false;
			bool HasDepthAttachment = false;
			nvrhi::FramebufferHandle Framebuffer;
			nvrhi::TextureHandle ColorTexture;
			nvrhi::TextureHandle DepthTexture;
		};

		struct SwapchainImage
		{
			vk::Image NativeImage;
			nvrhi::TextureHandle Texture;
			TextureHandle DepthTarget;
			nvrhi::FramebufferHandle Framebuffer;
			vk::Semaphore PresentReady;
		};

		void Initialize()
		{
			if (!m_NativeWindow)
				throw std::runtime_error("Vulkan renderer requires a valid GLFW window");
			if (glfwVulkanSupported() != GLFW_TRUE)
				throw std::runtime_error("GLFW could not find a usable Vulkan loader and installable driver");

			InitializeVulkanLoader();
			CreateInstance();
			CreateSurface();
			SelectPhysicalDevice();
			CreateLogicalDevice();
			CreateNvrhiDevice();
			CreateFrameContexts();
			(void)RecreateSwapchain();
		}

		void InitializeVulkanLoader()
		{
			m_VulkanLoader = std::make_unique<vk::detail::DynamicLoader>();
			const auto GetInstanceProcAddress = m_VulkanLoader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
			if (!GetInstanceProcAddress)
				throw std::runtime_error("The Vulkan loader does not export vkGetInstanceProcAddr");

			VULKAN_HPP_DEFAULT_DISPATCHER.init(GetInstanceProcAddress);
			const uint32_t LoaderVersion = vk::enumerateInstanceVersion();
			if (LoaderVersion < VulkanApiVersion)
				throw std::runtime_error("Vulkan 1.3 is required; the installed loader exposes " + ApiVersionString(LoaderVersion));

			PF_CORE_INFO("Vulkan loader/API version: {0}", ApiVersionString(LoaderVersion));
		}

		void CreateInstance()
		{
			const auto AvailableExtensions = vk::enumerateInstanceExtensionProperties();
			uint32_t RequiredExtensionCount = 0;
			const char** RequiredExtensions = glfwGetRequiredInstanceExtensions(&RequiredExtensionCount);
			if (!RequiredExtensions || RequiredExtensionCount == 0)
				throw std::runtime_error("GLFW did not provide the Vulkan surface extensions required for this window");

			for (uint32_t Index = 0; Index < RequiredExtensionCount; ++Index)
			{
				if (!HasName(AvailableExtensions, RequiredExtensions[Index]))
					throw std::runtime_error(std::string("The Vulkan loader is missing GLFW-required instance extension ") + RequiredExtensions[Index]);
				if (!HasName(m_EnabledInstanceExtensions, RequiredExtensions[Index]))
					m_EnabledInstanceExtensions.push_back(RequiredExtensions[Index]);
			}

			m_DebugUtilsEnabled = HasName(AvailableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			if (m_DebugUtilsEnabled && !HasName(m_EnabledInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
				m_EnabledInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#if defined(PF_DEBUG)
			const auto AvailableLayers = vk::enumerateInstanceLayerProperties();
			if (HasName(AvailableLayers, "VK_LAYER_KHRONOS_validation"))
			{
				m_EnabledLayers.push_back("VK_LAYER_KHRONOS_validation");
				m_KhronosValidationEnabled = true;
			}
			else
			{
				PF_CORE_WARN("VK_LAYER_KHRONOS_validation is not installed; Vulkan debug-utils and NVRHI validation remain enabled where available");
			}
#endif

			vk::DebugUtilsMessengerCreateInfoEXT DebugCreateInfo;
			if (m_DebugUtilsEnabled)
			{
				vk::DebugUtilsMessageSeverityFlagsEXT MessageSeverities =
					vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
					vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
#if defined(PF_DEBUG)
				MessageSeverities |= vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo;
#endif
				// Subscribe to all categories so general warnings are not silently dropped.
				// Verbose messages are mapped by the callback but omitted by default because
				// they can be high-volume.
				DebugCreateInfo
					.setMessageSeverity(MessageSeverities)
					.setMessageType(
						vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
						vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
						vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance)
					.setPfnUserCallback(VulkanDebugCallback);
			}

			vk::ApplicationInfo ApplicationInfo;
			ApplicationInfo
				.setPApplicationName("PulseForge")
				.setApplicationVersion(VK_MAKE_API_VERSION(0, 0, 1, 0))
				.setPEngineName("PulseForge")
				.setEngineVersion(VK_MAKE_API_VERSION(0, 0, 1, 0))
				.setApiVersion(VulkanApiVersion);

			vk::InstanceCreateInfo CreateInfo;
			CreateInfo
				.setPApplicationInfo(&ApplicationInfo)
				.setPEnabledExtensionNames(m_EnabledInstanceExtensions)
				.setPEnabledLayerNames(m_EnabledLayers);
			if (m_DebugUtilsEnabled)
				CreateInfo.setPNext(&DebugCreateInfo);

			m_Instance = vk::createInstance(CreateInfo);
			VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Instance);

			if (m_DebugUtilsEnabled)
			{
				m_DebugMessenger = m_Instance.createDebugUtilsMessengerEXT(DebugCreateInfo);
				PF_CORE_INFO("Vulkan debug-utils messenger enabled");
			}
			if (m_KhronosValidationEnabled)
				PF_CORE_INFO("Vulkan validation layer enabled");
		}

		void CreateSurface()
		{
			VkSurfaceKHR Surface = VK_NULL_HANDLE;
			const VkResult Result = glfwCreateWindowSurface(
				static_cast<VkInstance>(m_Instance),
				m_NativeWindow,
				nullptr,
				&Surface);
			CheckVkResult(Result, "glfwCreateWindowSurface");
			m_Surface = vk::SurfaceKHR(Surface);
		}

		std::optional<DeviceCandidate> EvaluateDevice(vk::PhysicalDevice PhysicalDevice)
		{
			DeviceCandidate Candidate;
			Candidate.Device = PhysicalDevice;
			Candidate.Properties = PhysicalDevice.getProperties();
			if (Candidate.Properties.apiVersion < VulkanApiVersion)
				return std::nullopt;
			if (!SupportsDepth32Attachment(PhysicalDevice))
				return std::nullopt;

			const auto Extensions = PhysicalDevice.enumerateDeviceExtensionProperties();
			if (!HasName(Extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
				return std::nullopt;

			const auto QueueProperties = PhysicalDevice.getQueueFamilyProperties();
			std::vector<VulkanSupport::QueueFamily> QueueFamilies;
			QueueFamilies.reserve(QueueProperties.size());
			for (uint32_t Index = 0; Index < QueueProperties.size(); ++Index)
			{
				const auto& Queue = QueueProperties[Index];
				const bool SupportsPresent = PhysicalDevice.getSurfaceSupportKHR(Index, m_Surface);
				QueueFamilies.push_back({
					Index,
					Queue.queueCount,
					(Queue.queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{},
					SupportsPresent
				});
			}
			auto QueueSelection = VulkanSupport::ChooseQueueFamilies(QueueFamilies);
			if (!QueueSelection)
				return std::nullopt;

			const auto SurfaceCapabilities = PhysicalDevice.getSurfaceCapabilitiesKHR(m_Surface);
			if (!HasRequiredSurfaceUsage(SurfaceCapabilities))
				return std::nullopt;

			auto SurfaceFormat = ChooseSupportedSurfaceFormat(PhysicalDevice.getSurfaceFormatsKHR(m_Surface));
			if (!SurfaceFormat)
				return std::nullopt;

			const auto VkPresentModes = PhysicalDevice.getSurfacePresentModesKHR(m_Surface);
			std::vector<VulkanSupport::PresentMode> PresentModes;
			PresentModes.reserve(VkPresentModes.size());
			for (vk::PresentModeKHR Mode : VkPresentModes)
				PresentModes.push_back(ToSupportPresentMode(Mode));
			auto PresentMode = VulkanSupport::ChoosePresentMode(PresentModes, true);
			if (!PresentMode)
				return std::nullopt;

			vk::PhysicalDeviceVulkan12Features Supported12;
			vk::PhysicalDeviceVulkan13Features Supported13;
			Supported12.setPNext(&Supported13);
			vk::PhysicalDeviceFeatures2 SupportedFeatures;
			SupportedFeatures.setPNext(&Supported12);
			PhysicalDevice.getFeatures2(&SupportedFeatures);
			const VulkanSupport::RequiredDeviceFeatures RequiredFeatures{
				Supported12.timelineSemaphore == VK_TRUE,
				Supported13.synchronization2 == VK_TRUE,
				Supported13.dynamicRendering == VK_TRUE
			};
			if (!VulkanSupport::SupportsRequiredDeviceFeatures(RequiredFeatures))
				return std::nullopt;

			Candidate.QueueFamilies = *QueueSelection;
			Candidate.SurfaceFormat = SurfaceFormat->first;
			Candidate.NvrhiFormat = SurfaceFormat->second;
			Candidate.PresentMode = ToVkPresentMode(*PresentMode);
			Candidate.Score = 0;
			switch (Candidate.Properties.deviceType)
			{
				case vk::PhysicalDeviceType::eDiscreteGpu: Candidate.Score += 300; break;
				case vk::PhysicalDeviceType::eIntegratedGpu: Candidate.Score += 200; break;
				case vk::PhysicalDeviceType::eVirtualGpu: Candidate.Score += 100; break;
				default: break;
			}
			if (Candidate.QueueFamilies.GraphicsIndex == Candidate.QueueFamilies.PresentIndex)
				Candidate.Score += 25;

			return Candidate;
		}

		void SelectPhysicalDevice()
		{
			const auto PhysicalDevices = m_Instance.enumeratePhysicalDevices();
			if (PhysicalDevices.empty())
				throw std::runtime_error("Vulkan reported no physical devices");

			std::optional<DeviceCandidate> BestCandidate;
			for (vk::PhysicalDevice PhysicalDevice : PhysicalDevices)
			{
				auto Candidate = EvaluateDevice(PhysicalDevice);
				if (Candidate && (!BestCandidate || Candidate->Score > BestCandidate->Score))
					BestCandidate = std::move(Candidate);
			}

			if (!BestCandidate)
			{
				throw std::runtime_error(
					"No Vulkan 1.3 GPU supports graphics/presentation, VK_KHR_swapchain, "
					"timeline semaphores, synchronization2, dynamic rendering, D32 depth attachments, "
					"a recognized swapchain format, and color/transfer-destination images");
			}

			m_PhysicalDevice = BestCandidate->Device;
			m_QueueFamilies = BestCandidate->QueueFamilies;
			m_SurfaceFormat = BestCandidate->SurfaceFormat;
			m_NvrhiFormat = BestCandidate->NvrhiFormat;
			m_PresentMode = BestCandidate->PresentMode;

			PF_CORE_INFO(
				"Selected Vulkan GPU: {0} (API {1}, driver {2}, graphics queue {3}, present queue {4})",
				std::string(BestCandidate->Properties.deviceName.data()),
				ApiVersionString(BestCandidate->Properties.apiVersion),
				BestCandidate->Properties.driverVersion,
				m_QueueFamilies.GraphicsIndex,
				m_QueueFamilies.PresentIndex);
		}

		void CreateLogicalDevice()
		{
			constexpr float QueuePriority = 1.0f;
			std::vector<vk::DeviceQueueCreateInfo> QueueCreateInfos;
			QueueCreateInfos.emplace_back(vk::DeviceQueueCreateInfo()
				.setQueueFamilyIndex(m_QueueFamilies.GraphicsIndex)
				.setQueuePriorities(vk::ArrayProxyNoTemporaries<const float>(1, &QueuePriority)));
			if (m_QueueFamilies.PresentIndex != m_QueueFamilies.GraphicsIndex)
			{
				QueueCreateInfos.emplace_back(vk::DeviceQueueCreateInfo()
					.setQueueFamilyIndex(m_QueueFamilies.PresentIndex)
					.setQueuePriorities(vk::ArrayProxyNoTemporaries<const float>(1, &QueuePriority)));
			}

			vk::PhysicalDeviceVulkan12Features Enabled12;
			Enabled12.setTimelineSemaphore(VK_TRUE);
			vk::PhysicalDeviceVulkan13Features Enabled13;
			Enabled13
				.setSynchronization2(VK_TRUE)
				.setDynamicRendering(VK_TRUE);
			Enabled12.setPNext(&Enabled13);

			std::vector<const char*> DeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
			vk::DeviceCreateInfo CreateInfo;
			CreateInfo
				.setPNext(&Enabled12)
				.setQueueCreateInfos(QueueCreateInfos)
				.setPEnabledExtensionNames(DeviceExtensions);

			m_Device = m_PhysicalDevice.createDevice(CreateInfo);
			VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Device);
			m_GraphicsQueue = m_Device.getQueue(m_QueueFamilies.GraphicsIndex, 0);
			m_PresentQueue = m_Device.getQueue(m_QueueFamilies.PresentIndex, 0);
		}

		void CreateNvrhiDevice()
		{
			std::vector<const char*> DeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
			nvrhi::vulkan::DeviceDesc Description;
			Description.errorCB = &m_NvrhiMessageCallback;
			Description.instance = static_cast<VkInstance>(m_Instance);
			Description.physicalDevice = static_cast<VkPhysicalDevice>(m_PhysicalDevice);
			Description.device = static_cast<VkDevice>(m_Device);
			Description.graphicsQueue = static_cast<VkQueue>(m_GraphicsQueue);
			Description.graphicsQueueIndex = static_cast<int>(m_QueueFamilies.GraphicsIndex);
			Description.deviceExtensions = DeviceExtensions.data();
			Description.numDeviceExtensions = DeviceExtensions.size();
			Description.instanceExtensions = m_EnabledInstanceExtensions.data();
			Description.numInstanceExtensions = m_EnabledInstanceExtensions.size();

			m_VulkanDevice = nvrhi::vulkan::createDevice(Description);
			if (!m_VulkanDevice)
				throw std::runtime_error("NVRHI could not wrap the initialized Vulkan device and graphics queue");

			m_ActiveNvrhiDevice = m_VulkanDevice.Get();
#if defined(PF_DEBUG)
			m_ValidationDevice = nvrhi::validation::createValidationLayer(m_VulkanDevice.Get());
			if (m_ValidationDevice)
			{
				m_ActiveNvrhiDevice = m_ValidationDevice.Get();
				PF_CORE_INFO("NVRHI validation layer enabled");
			}
#endif
		}

		void CreateFrameContexts()
		{
			const vk::SemaphoreCreateInfo CreateInfo;
			for (FrameContext& Frame : m_Frames)
			{
				Frame.ImageAvailable = m_Device.createSemaphore(CreateInfo);
				Frame.CommandList = m_ActiveNvrhiDevice->createCommandList();
				if (!Frame.CommandList)
					throw std::runtime_error("NVRHI could not create a Vulkan frame command list");
			}
		}

		void WaitForFrame(const FrameContext& Frame)
		{
			if (Frame.SubmissionValue == 0)
				return;

			const vk::Semaphore CompletionSemaphore(m_VulkanDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics));
			const std::array<vk::Semaphore, 1> Semaphores = { CompletionSemaphore };
			const std::array<uint64_t, 1> Values = { Frame.SubmissionValue };
			const auto WaitInfo = vk::SemaphoreWaitInfo()
				.setSemaphores(Semaphores)
				.setValues(Values);
			const vk::Result WaitResult = m_Device.waitSemaphores(WaitInfo, std::numeric_limits<uint64_t>::max());
			if (WaitResult != vk::Result::eSuccess)
				throw std::runtime_error("Vulkan frame synchronization wait failed: " + vk::to_string(WaitResult));
		}

		std::optional<vk::Extent2D> ChooseSwapchainExtent(const vk::SurfaceCapabilitiesKHR& Capabilities) const
		{
			const auto [Width, Height] = m_Window.GetFramebufferSize();
			const bool HasFixedExtent = Capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max();
			const auto Extent = VulkanSupport::ChooseExtent(
				{ Width, Height },
				{ Capabilities.currentExtent.width, Capabilities.currentExtent.height },
				HasFixedExtent,
				{ Capabilities.minImageExtent.width, Capabilities.minImageExtent.height },
				{ Capabilities.maxImageExtent.width, Capabilities.maxImageExtent.height });
			if (Extent.Width == 0 || Extent.Height == 0)
				return std::nullopt;
			return vk::Extent2D(Extent.Width, Extent.Height);
		}

		void ReleaseSwapchainImages()
		{
			for (SwapchainImage& Image : m_SwapchainImages)
			{
				if (Image.PresentReady)
					m_Device.destroySemaphore(Image.PresentReady);
				Image.PresentReady = vk::Semaphore();
			}
			m_SwapchainImages.clear();
			if (m_ActiveNvrhiDevice)
				m_ActiveNvrhiDevice->runGarbageCollection();
		}

		bool RecreateSwapchain()
		{
			if (!m_Device)
				return false;

			const auto [FramebufferWidth, FramebufferHeight] = m_Window.GetFramebufferSize();
			if (FramebufferWidth == 0 || FramebufferHeight == 0)
				return false;

			if (m_ActiveNvrhiDevice && !m_ActiveNvrhiDevice->waitForIdle())
				throw std::runtime_error("NVRHI failed while waiting for Vulkan graphics work before swapchain recreation");
			m_Device.waitIdle();
			ReleaseSwapchainImages();

			const auto Capabilities = m_PhysicalDevice.getSurfaceCapabilitiesKHR(m_Surface);
			auto Extent = ChooseSwapchainExtent(Capabilities);
			if (!Extent)
				return false;

			const uint32_t ImageCount = VulkanSupport::ChooseImageCount(
				Capabilities.minImageCount,
				Capabilities.maxImageCount);
			const vk::ImageUsageFlags ImageUsage =
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
			const std::array<uint32_t, 2> QueueIndices = {
				m_QueueFamilies.GraphicsIndex,
				m_QueueFamilies.PresentIndex
			};
			const bool SeparateQueues = QueueIndices[0] != QueueIndices[1];
			auto PresentInfo = vk::SwapchainCreateInfoKHR()
				.setSurface(m_Surface)
				.setMinImageCount(ImageCount)
				.setImageFormat(m_SurfaceFormat.format)
				.setImageColorSpace(m_SurfaceFormat.colorSpace)
				.setImageExtent(*Extent)
				.setImageArrayLayers(1)
				.setImageUsage(ImageUsage)
				.setImageSharingMode(SeparateQueues ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive)
				.setPreTransform(Capabilities.currentTransform)
				.setCompositeAlpha(ChooseCompositeAlpha(Capabilities.supportedCompositeAlpha))
				.setPresentMode(m_PresentMode)
				.setClipped(VK_TRUE);
			if (SeparateQueues)
				PresentInfo.setQueueFamilyIndices(QueueIndices);

			// Avoid the oldSwapchain handoff path, which retained private memory per recreation on tested Vulkan ICDs.
			// Queue work is idle and all NVRHI wrappers for the retired chain have already been released.
			if (m_Swapchain)
			{
				m_Device.destroySwapchainKHR(m_Swapchain);
				m_Swapchain = vk::SwapchainKHR();
			}
			m_Swapchain = m_Device.createSwapchainKHR(PresentInfo);
			m_SwapchainExtent = *Extent;

			const auto NativeImages = m_Device.getSwapchainImagesKHR(m_Swapchain);
			if (NativeImages.empty())
				throw std::runtime_error("Vulkan created a swapchain without any images");
			m_SwapchainImages.reserve(NativeImages.size());
			for (size_t Index = 0; Index < NativeImages.size(); ++Index)
			{
				m_SwapchainImages.emplace_back();
				SwapchainImage& Image = m_SwapchainImages.back();
				Image.NativeImage = NativeImages[Index];
				Image.PresentReady = m_Device.createSemaphore(vk::SemaphoreCreateInfo());

				nvrhi::TextureDesc TextureDescription;
				TextureDescription
					.setDimension(nvrhi::TextureDimension::Texture2D)
					.setFormat(m_NvrhiFormat)
					.setWidth(m_SwapchainExtent.width)
					.setHeight(m_SwapchainExtent.height)
					.setIsRenderTarget(true)
					.enableAutomaticStateTracking(nvrhi::ResourceStates::Present)
					.setDebugName("PulseForge Vulkan swapchain image " + std::to_string(Index));
				Image.Texture = m_ActiveNvrhiDevice->createHandleForNativeTexture(
					nvrhi::ObjectTypes::VK_Image,
					nvrhi::Object(ToNativeHandleValue(static_cast<VkImage>(Image.NativeImage))),
					TextureDescription);
				if (!Image.Texture)
					throw std::runtime_error("NVRHI could not wrap a Vulkan swapchain image");

				TextureDesc DepthDescription;
				DepthDescription.Width = m_SwapchainExtent.width;
				DepthDescription.Height = m_SwapchainExtent.height;
				DepthDescription.Format = TextureFormat::Depth32Float;
				DepthDescription.Usage = TextureUsage::DepthStencilAttachment;
				DepthDescription.DebugName = "PulseForge swapchain depth image " + std::to_string(Index);
				auto CreatedDepthTarget = CreateTexture(DepthDescription, std::span<const TextureSubresourceData>{});
				if (!CreatedDepthTarget)
					throw std::runtime_error("Could not create a Vulkan swapchain depth target: " + CreatedDepthTarget.error().Message);
				Image.DepthTarget = std::move(CreatedDepthTarget.value());
				const auto* DepthTarget = dynamic_cast<const VulkanTexture*>(Image.DepthTarget.get());
				if (!DepthTarget)
					throw std::runtime_error("Vulkan backend returned an incompatible depth-target implementation");

				nvrhi::FramebufferDesc FramebufferDescription;
				FramebufferDescription.addColorAttachment(Image.Texture);
				FramebufferDescription.setDepthAttachment(DepthTarget->GetNativeTexture());
				Image.Framebuffer = m_ActiveNvrhiDevice->createFramebuffer(FramebufferDescription);
				if (!Image.Framebuffer)
					throw std::runtime_error("NVRHI could not create a framebuffer for a Vulkan swapchain image");
			}

			m_SwapchainRecreationNeeded = false;
			m_LoggedFirstPresentedFrameForSwapchain = false;
			PF_CORE_INFO(
				"Vulkan swapchain: {0}x{1}, {2} images, format {3}, present mode {4} (VSync enabled)",
				m_SwapchainExtent.width,
				m_SwapchainExtent.height,
				m_SwapchainImages.size(),
				vk::to_string(m_SurfaceFormat.format),
				vk::to_string(m_PresentMode));
			return true;
		}

		void Shutdown() noexcept
		{
			m_FrameActive = false;
			if (m_Device)
			{
				try
				{
					if (m_ActiveNvrhiDevice)
						(void)m_ActiveNvrhiDevice->waitForIdle();
					m_Device.waitIdle();
				}
				catch (const std::exception& Exception)
				{
					PF_CORE_ERROR("Vulkan shutdown wait failed: {0}", Exception.what());
				}
			}
			m_ActiveRenderTarget.reset();

			if (m_Device)
			{
				ReleaseSwapchainImages();
				for (FrameContext& Frame : m_Frames)
					Frame.CommandList = nullptr;
				m_ValidationDevice = nullptr;
				m_ActiveNvrhiDevice = nullptr;
				m_VulkanDevice = nullptr;

				for (FrameContext& Frame : m_Frames)
				{
					if (Frame.ImageAvailable)
						m_Device.destroySemaphore(Frame.ImageAvailable);
					Frame.ImageAvailable = vk::Semaphore();
				}

				if (m_Swapchain)
					m_Device.destroySwapchainKHR(m_Swapchain);
				m_Swapchain = vk::SwapchainKHR();
				m_Device.destroy();
				m_Device = vk::Device();
			}

			if (m_Instance && m_Surface)
				m_Instance.destroySurfaceKHR(m_Surface);
			m_Surface = vk::SurfaceKHR();

			if (m_Instance && m_DebugMessenger)
				m_Instance.destroyDebugUtilsMessengerEXT(m_DebugMessenger);
			m_DebugMessenger = vk::DebugUtilsMessengerEXT();

			if (m_Instance)
				m_Instance.destroy();
			m_Instance = vk::Instance();

			m_PhysicalDevice = vk::PhysicalDevice();
			m_GraphicsQueue = vk::Queue();
			m_PresentQueue = vk::Queue();
			m_VulkanLoader.reset();
		}

	private:
		Window& m_Window;
		GLFWwindow* m_NativeWindow = nullptr;
		std::unique_ptr<vk::detail::DynamicLoader> m_VulkanLoader;
		NvrhiMessageCallback m_NvrhiMessageCallback;

		vk::Instance m_Instance;
		vk::DebugUtilsMessengerEXT m_DebugMessenger;
		vk::SurfaceKHR m_Surface;
		vk::PhysicalDevice m_PhysicalDevice;
		vk::Device m_Device;
		vk::Queue m_GraphicsQueue;
		vk::Queue m_PresentQueue;
		VulkanSupport::QueueFamilySelection m_QueueFamilies;

		nvrhi::vulkan::DeviceHandle m_VulkanDevice;
		nvrhi::DeviceHandle m_ValidationDevice;
		nvrhi::IDevice* m_ActiveNvrhiDevice = nullptr;
		std::vector<const char*> m_EnabledInstanceExtensions;
		std::vector<const char*> m_EnabledLayers;
		std::array<FrameContext, FramesInFlight> m_Frames;
		std::optional<ActiveRenderTarget> m_ActiveRenderTarget;
		std::vector<SwapchainImage> m_SwapchainImages;
		vk::SwapchainKHR m_Swapchain;
		vk::Extent2D m_SwapchainExtent;
		vk::SurfaceFormatKHR m_SurfaceFormat;
		nvrhi::Format m_NvrhiFormat = nvrhi::Format::UNKNOWN;
		vk::PresentModeKHR m_PresentMode = vk::PresentModeKHR::eFifo;
		uint32_t m_CurrentFrame = 0;
		uint32_t m_AcquiredImageIndex = 0;
		bool m_DebugUtilsEnabled = false;
		bool m_KhronosValidationEnabled = false;
		bool m_SwapchainRecreationNeeded = false;
		bool m_AcquiredSuboptimal = false;
		bool m_LoggedFirstPresentedFrameForSwapchain = false;
		bool m_LoggedFirstDraw = false;
		bool m_FrameActive = false;
	};

	std::unique_ptr<RendererBackend> CreateVulkanRenderer(Window& Window)
	{
		return std::make_unique<VulkanRenderer>(Window);
	}
}
