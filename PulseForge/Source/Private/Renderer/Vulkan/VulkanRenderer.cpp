#ifndef VK_ENABLE_BETA_EXTENSIONS
	#define VK_ENABLE_BETA_EXTENSIONS 1
#endif
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

#include "Renderer/RendererBackend.h"
#include "Renderer/Vulkan/VulkanPresentationResources.h"
#include "Renderer/Vulkan/VulkanRendererShutdown.h"
#include "Renderer/Vulkan/VulkanResourceObjects.h"
#include "Renderer/Vulkan/VulkanSupport.h"
#include "Window/Platform/GLFW/GLFWWindow.h"
#include "Window/Window.h"

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <type_traits>

namespace PulseForge
{
	using namespace VulkanDetail;

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

		void AppendUniqueName(std::vector<const char*>& Names, const char* Name)
		{
			if (!HasName(Names, Name)) Names.push_back(Name);
		}

		VulkanSupport::PresentationQueueResult ClassifyPresentationQueueResult(VkResult Result)
		{
			switch (Result)
			{
			case VK_SUCCESS:
				return VulkanSupport::PresentationQueueResult::Success;
			case VK_SUBOPTIMAL_KHR:
				return VulkanSupport::PresentationQueueResult::Suboptimal;
			case VK_ERROR_OUT_OF_DATE_KHR:
				return VulkanSupport::PresentationQueueResult::OutOfDate;
			case VK_ERROR_SURFACE_LOST_KHR:
				return VulkanSupport::PresentationQueueResult::SurfaceLost;
			case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
				return VulkanSupport::PresentationQueueResult::FullscreenExclusiveLost;
			case VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT:
				return VulkanSupport::PresentationQueueResult::PresentTimingQueueFull;
			case VK_ERROR_OUT_OF_HOST_MEMORY:
				return VulkanSupport::PresentationQueueResult::OutOfHostMemory;
			case VK_ERROR_OUT_OF_DEVICE_MEMORY:
				return VulkanSupport::PresentationQueueResult::OutOfDeviceMemory;
			case VK_ERROR_DEVICE_LOST:
				return VulkanSupport::PresentationQueueResult::DeviceLost;
			default:
				return VulkanSupport::PresentationQueueResult::OtherFailure;
			}
		}

		VulkanSupport::PresentationResultHandling RecordPresentationResult(VulkanSwapchainImage& Image, VkResult Result)
		{
			const VulkanSupport::PresentationResultHandling Handling =
				VulkanSupport::ResolvePresentationResult(ClassifyPresentationQueueResult(Result));
			if (VulkanSupport::IsSharedDeviceLoss(ClassifyPresentationQueueResult(Result)))
			{
				Image.PresentationState = PresentationRequestState::DeviceLostIndeterminate;
				return Handling;
			}
			switch (Handling.RequestDisposition)
			{
			case VulkanSupport::PresentationRequestDisposition::Enqueued:
				Image.PresentationState = PresentationRequestState::Pending;
				break;
			case VulkanSupport::PresentationRequestDisposition::NotEnqueued:
				Image.PresentationState = PresentationRequestState::NotSubmitted;
				break;
			case VulkanSupport::PresentationRequestDisposition::Indeterminate:
				Image.PresentationState = PresentationRequestState::Indeterminate;
				break;
			}
			return Handling;
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
				vk::Format::eR8G8B8A8Srgb,
				vk::Format::eB8G8R8A8Unorm,
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
			void SetDeviceHealth(VulkanDeviceHealth* DeviceHealth) noexcept { m_DeviceHealth = DeviceHealth; }

			void message(nvrhi::MessageSeverity Severity, const char* Text) override
			{
				const char* SafeText = Text ? Text : "NVRHI reported an empty diagnostic";
				// The pinned NVRHI queue reports a swallowed vk::DeviceLostError through this callback text.
				if ((Severity == nvrhi::MessageSeverity::Error || Severity == nvrhi::MessageSeverity::Fatal) &&
					std::strcmp(SafeText, "Device Removed!") == 0 && m_DeviceHealth)
					m_DeviceHealth->MarkLost();
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

		private:
			VulkanDeviceHealth* m_DeviceHealth = nullptr;
		};

		struct DeviceCandidate
		{
			vk::PhysicalDevice Device;
			vk::PhysicalDeviceProperties Properties;
			VulkanSupport::QueueFamilySelection QueueFamilies;
			vk::SurfaceFormatKHR SurfaceFormat;
			nvrhi::Format NvrhiFormat = nvrhi::Format::UNKNOWN;
			vk::PresentModeKHR PresentMode = vk::PresentModeKHR::eFifo;
			VulkanSupport::PresentationDeviceExtensionAvailability AvailablePresentationDeviceExtensions;
			bool PortabilitySubsetExtensionAvailable = false;
			uint32_t MinVertexInputBindingStrideAlignment = 1;
			bool FillModeNonSolidSupported = false;
			bool PresentationFeatureSupported = false;
			VulkanSupport::PresentationRetirementMechanism PresentationRetirementMechanism =
				VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort;
			VulkanSupport::PresentationFenceCapabilityReason PresentationCapabilityReason =
				VulkanSupport::PresentationFenceCapabilityReason::MissingSurfaceExtension;
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
	} // namespace

	class VulkanRenderer final : public RendererBackend, private RendererShutdownOperations
	{
	public:
		explicit VulkanRenderer(Window& Window)
			: m_Window(Window),
			  m_NativeWindow(static_cast<GLFWwindow*>(Window.GetNativeWindow()))
		{
			m_NvrhiMessageCallback.SetDeviceHealth(&m_DeviceHealth);
			try
			{
				Initialize();
			}
			catch (const vk::DeviceLostError& Exception)
			{
				MarkDeviceLost(VK_ERROR_DEVICE_LOST);
				PF_CORE_ERROR("Vulkan renderer initialization failed after device loss: {0}", Exception.what());
				const ShutdownResult ShutdownStatus = Shutdown();
				if (!ShutdownStatus.IsComplete()) FailStopUnsafeShutdown(ShutdownStatus);
				throw;
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				PF_CORE_ERROR("Vulkan renderer initialization failed: {0}", Exception.what());
				const ShutdownResult ShutdownStatus = Shutdown();
				if (!ShutdownStatus.IsComplete()) FailStopUnsafeShutdown(ShutdownStatus);
				throw;
			}
			catch (...)
			{
				PF_CORE_ERROR("Vulkan renderer initialization failed with a non-standard exception");
				const ShutdownResult ShutdownStatus = Shutdown();
				if (!ShutdownStatus.IsComplete()) FailStopUnsafeShutdown(ShutdownStatus);
				throw;
			}
		}

		~VulkanRenderer() override
		{
			const ShutdownResult ShutdownStatus = Shutdown();
			if (!ShutdownStatus.IsComplete()) FailStopUnsafeShutdown(ShutdownStatus);
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
			m_DeviceHealth.EnsureOperational();
			m_RetirePreviousGenerationsAfterSubmit = false;
			if (!m_Device)
				throw std::runtime_error("Vulkan renderer is not initialized");
			if (m_SwapchainRetirementBlocked)
				return false;
			if (m_ActivePlatformTarget)
			{
				PF_CORE_ERROR("Cannot begin the main Vulkan frame while a native presentation target is active");
				return false;
			}

			const auto [FramebufferWidth, FramebufferHeight] = m_Window.GetFramebufferSize();
			if (FramebufferWidth == 0 || FramebufferHeight == 0)
				return false;
#if defined(PF_DEBUG)
			if (m_DebugMainAcquireOutOfDateAfterNativeTargetFrames > 0 &&
				--m_DebugMainAcquireOutOfDateAfterNativeTargetFrames == 0)
			{
				m_DebugMainAcquireOutOfDateRemaining = static_cast<uint32_t>(m_DebugMainAcquireOutOfDateAfterNativeTargetCount);
				PF_CORE_WARN(
					"Debug Vulkan presentation test will now inject {} main swapchain out-of-date acquisitions",
					m_DebugMainAcquireOutOfDateRemaining);
			}
#endif
			const bool RecreationRequested = !m_Swapchain || m_SwapchainRecreationNeeded ||
				FramebufferWidth != m_SwapchainExtent.width || FramebufferHeight != m_SwapchainExtent.height;
			const bool CanCreateGeneration =
				VulkanSupport::CanCreateSwapchainGeneration(UsesPresentationFences(), m_Swapchain != vk::SwapchainKHR(),
					m_RetiredSwapchainGenerations.size(), m_SwapchainUnusable, m_LegacyRecoveryAttemptedAtLimit);
			if (RecreationRequested && !CanCreateGeneration &&
				(m_SwapchainUnusable || !m_Swapchain))
			{
				m_SwapchainRetirementBlocked = true;
				if (!m_SwapchainRetirementBlockedLogged)
				{
					PF_CORE_ERROR("Main Vulkan presentation is terminally blocked for this renderer instance: no usable current swapchain "
								  "can be presented and {} retired generations still await a specification-backed reacquire completion; "
								  "the document remains open and renderer restart is required",
						m_RetiredSwapchainGenerations.size());
					m_SwapchainRetirementBlockedLogged = true;
				}
				return false;
			}
			const auto FrameStartAction = VulkanSupport::ResolveSwapchainFrameStartAction(
				m_Swapchain != vk::SwapchainKHR(),
				RecreationRequested,
				CanCreateGeneration,
				!m_SwapchainUnusable);
			if (FrameStartAction == VulkanSupport::SwapchainFrameStartAction::Skip)
				return false;
			if (RecreationRequested && !CanCreateGeneration && !m_DeferredRetirementLimitLogged)
			{
				PF_CORE_ERROR("Main swapchain recreation is deferred while the current generation renders because {} legacy presentation "
							  "generations are still awaiting reacquisition",
					m_RetiredSwapchainGenerations.size());
				m_DeferredRetirementLimitLogged = true;
			}
			if (FrameStartAction == VulkanSupport::SwapchainFrameStartAction::Recreate)
			{
				if (!RecreateSwapchain(m_SwapchainUnusable) && (!m_Swapchain || m_SwapchainUnusable))
					return false;
			}

			FrameContext& Frame = m_Frames[m_CurrentFrame];
			WaitForFrame(Frame);

			uint32_t ImageIndex = 0;
			VkResult AcquireResult = VK_SUCCESS;
#if defined(PF_DEBUG)
			if (m_DebugMainAcquireOutOfDateRemaining > 0)
			{
				--m_DebugMainAcquireOutOfDateRemaining;
				AcquireResult = VK_ERROR_OUT_OF_DATE_KHR;
				PF_CORE_INFO("Debug fixture injected main swapchain out-of-date acquire; {} injections remain",
					m_DebugMainAcquireOutOfDateRemaining);
			}
			else
#endif
			AcquireResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(
				static_cast<VkDevice>(m_Device),
				static_cast<VkSwapchainKHR>(m_Swapchain),
				std::numeric_limits<uint64_t>::max(),
				static_cast<VkSemaphore>(Frame.ImageAvailable),
				VK_NULL_HANDLE,
				&ImageIndex);

			if (AcquireResult == VK_ERROR_OUT_OF_DATE_KHR)
			{
				m_SwapchainRecreationNeeded = true;
				m_SwapchainUnusable = true;
				(void)RecreateSwapchain(true);
				return false;
			}
			if (AcquireResult != VK_SUCCESS && AcquireResult != VK_SUBOPTIMAL_KHR)
				CheckVkResult(AcquireResult, "vkAcquireNextImageKHR");

			if (ImageIndex >= m_SwapchainImages.size())
				throw std::runtime_error("Vulkan returned a swapchain image index outside the image array");

			m_AcquiredImageIndex = ImageIndex;
			m_AcquiredSuboptimal = AcquireResult == VK_SUBOPTIMAL_KHR;
			SwapchainImage& Image = m_SwapchainImages[m_AcquiredImageIndex];
			m_RetirePreviousGenerationsAfterSubmit =
				!UsesPresentationFences() && m_FirstPresentationPending && m_FirstPresentationImageIndex == m_AcquiredImageIndex;
			m_PresentationResources.WaitForPresentation(Image);
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
			m_Recording = RecordingContext{
				&Frame,
				&m_SwapchainImages,
				m_SwapchainExtent,
				m_AcquiredImageIndex,
				m_Swapchain,
				m_PresentQueue,
				true
			};
			m_MainRecording = m_Recording;
			m_FrameActive = true;
			return true;
		}

		void EndFrame() override
		{
			if (!m_FrameActive)
				return;
			m_DeviceHealth.EnsureOperational();
			m_Recording = m_MainRecording;

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
			m_DeviceHealth.EnsureOperational();
			Frame.SubmissionValue =
				ExecuteNvrhiCommandList(Frame.CommandList, nvrhi::CommandQueue::Graphics, "submitting the main presentation frame");
			if (m_RetirePreviousGenerationsAfterSubmit &&
				VulkanSupport::ShouldRetireDeferredSwapchains(
					UsesPresentationFences(), m_FirstPresentationPending, m_FirstPresentationImageIndex, m_AcquiredImageIndex, true))
			{
				// Waiting this one graphics submission consumes the acquire semaphore. The
				// Khronos fallback proof then permits retirement of generations older than
				// the first presentation on the current swapchain.
				const size_t RetiredCount = m_RetiredSwapchainGenerations.size();
				WaitForFrame(Frame);
				RetireMainSwapchainsAfterReacquire();
				PF_CORE_INFO("Main Vulkan presentation reacquire retired {} legacy swapchain generations",
					RetiredCount);
				m_RetirePreviousGenerationsAfterSubmit = false;
			}

			m_PresentationResources.PrepareFenceForReuse(Image);
			const VkSemaphore PresentSemaphore = static_cast<VkSemaphore>(Image.PresentReady);
			const VkSwapchainKHR Swapchain = static_cast<VkSwapchainKHR>(m_Swapchain);
			const VkFence NativePresentFence = static_cast<VkFence>(Image.PresentFence);
			VkSwapchainPresentFenceInfoKHR PresentFenceInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR, nullptr, 1,
				UsesPresentationFences() ? &NativePresentFence : nullptr };
			VkPresentInfoKHR PresentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, UsesPresentationFences() ? &PresentFenceInfo : nullptr, 1,
				&PresentSemaphore, 1, &Swapchain, &m_AcquiredImageIndex, nullptr };
			const VkResult PresentResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(
				static_cast<VkQueue>(m_PresentQueue),
				&PresentInfo);
			if (PresentResult == VK_ERROR_DEVICE_LOST) MarkDeviceLost(PresentResult);
			const VulkanSupport::PresentationResultHandling PresentationHandling = RecordPresentationResult(Image, PresentResult);
			if (m_DeviceHealth.IsLost())
			{
				ClearActiveRecordingState();
				throw std::runtime_error("Vulkan device was lost while presenting the main swapchain image");
			}
			if (!UsesPresentationFences() && !m_FirstPresentationPending && !m_FirstPresentationCompleted &&
				(PresentResult == VK_SUCCESS || PresentResult == VK_SUBOPTIMAL_KHR))
			{
				m_FirstPresentationPending = true;
				m_FirstPresentationImageIndex = m_AcquiredImageIndex;
			}
			RunNvrhiGarbageCollection();

			const bool AcquiredSuboptimal = m_AcquiredSuboptimal;
			m_FrameActive = false;
			m_Recording = {};
			m_MainRecording = {};
			m_AcquiredSuboptimal = false;
			m_CurrentFrame = (m_CurrentFrame + 1) % m_Frames.size();
			m_SwapchainUnusable = !PresentationHandling.CurrentSwapchainUsable;
			if (PresentResult == VK_SUCCESS || PresentResult == VK_SUBOPTIMAL_KHR)
			{
				m_SwapchainUnusable = false;
				m_SwapchainRetirementBlocked = false;
				m_SwapchainRetirementBlockedLogged = false;
			}
#if defined(PF_DEBUG)
			if (PresentResult == VK_SUCCESS || PresentResult == VK_SUBOPTIMAL_KHR)
			{
				for (const auto& [TargetID, OtherHost] : m_PlatformTargets)
					if (OtherHost && OtherHost->RetirementBlocked)
					{
						if (m_MainFramesDuringNativeRetirementBlock < 60)
							++m_MainFramesDuringNativeRetirementBlock;
						if (m_MainFramesDuringNativeRetirementBlock == 60 &&
							!m_LoggedMainPresentationDuringNativeRetirementBlock)
						{
							m_LoggedMainPresentationDuringNativeRetirementBlock = true;
							PF_CORE_INFO(
								"Main Vulkan presentation remained active for 60 frames after native host {} became terminally blocked",
								TargetID);
						}
						break;
					}
			}
#endif

			if (PresentationHandling.RecoveryAction == VulkanSupport::PresentationRecoveryAction::BlockHost)
			{
				PF_CORE_ERROR("Main Vulkan presentation stopped after vkQueuePresentKHR returned {}; request was {} and this "
							  "swapchain will not be reused. Shutdown must complete the submitted frame before releasing its resources",
					vk::to_string(static_cast<vk::Result>(PresentResult)),
					VulkanSupport::ToString(PresentationHandling.RequestDisposition));
				CheckVkResult(PresentResult, "vkQueuePresentKHR");
			}

			if (PresentResult == VK_SUCCESS && !m_LoggedFirstPresentedFrameForSwapchain)
			{
				m_LoggedFirstPresentedFrameForSwapchain = true;
				PF_CORE_INFO(
					"First Vulkan/NVRHI frame submitted and presented successfully (geometry draw recorded: {0})",
					m_LoggedFirstDraw);
			}

			if (AcquiredSuboptimal || PresentationHandling.RecoveryAction == VulkanSupport::PresentationRecoveryAction::RecreateSwapchain)
			{
				m_SwapchainRecreationNeeded = true;
				return;
			}
		}

		std::expected<uint64_t, std::string> CreatePlatformPresentationTarget(void* NativeWindow) override
		{
			if (m_DeviceHealth.IsLost()) return std::unexpected("The Vulkan device is lost; native presentation hosts cannot be created");
			auto* Window = static_cast<GLFWwindow*>(NativeWindow);
			if (!Window)
				return std::unexpected("The platform viewport does not expose a GLFW window");

			auto Host = std::make_unique<PlatformPresentationHost>();
			Host->Window = Window;
#if defined(PF_DEBUG)
			Host->InjectImageAllocationFailure = std::exchange(m_InjectNextPlatformImageAllocationFailure, false);
			Host->InjectAcquireOutOfDateCount = std::exchange(m_DebugNativeAcquireOutOfDateCount, 0u);
			Host->InjectSwapchainCreationFailureAtDeferredCount = std::exchange(m_DebugNativeSwapchainCreationFailureAtDeferredCount, 0u);
#endif
			const uint64_t TargetID = ++m_NextPlatformTargetID;
			auto [HostEntry, Inserted] = m_PlatformTargets.emplace(TargetID, std::move(Host));
			if (!Inserted) return std::unexpected("Could not reserve ownership for a native Vulkan presentation host");
			PlatformPresentationHost& Candidate = *HostEntry->second;

			try
			{
				VkSurfaceKHR NativeSurface = VK_NULL_HANDLE;
				const VkResult SurfaceResult =
					glfwCreateWindowSurface(static_cast<VkInstance>(m_Instance), Window, nullptr, &NativeSurface);
				if (NativeSurface != VK_NULL_HANDLE) Candidate.Surface = vk::SurfaceKHR(NativeSurface);
				if (SurfaceResult != VK_SUCCESS)
				{
					MarkDeviceLost(SurfaceResult);
					throw std::runtime_error("Could not create a Vulkan surface for the native editor window: " +
											 vk::to_string(static_cast<vk::Result>(SurfaceResult)));
				}

				if (!m_PhysicalDevice.getSurfaceSupportKHR(m_QueueFamilies.PresentIndex, Candidate.Surface))
					throw std::runtime_error("The active Vulkan present queue does not support this window surface");

				const auto SurfaceFormats = m_PhysicalDevice.getSurfaceFormatsKHR(Candidate.Surface);
				auto CompatibleFormat = std::find_if(
					SurfaceFormats.begin(), SurfaceFormats.end(), [this](const vk::SurfaceFormatKHR& Candidate)
					{
						const auto Format = ToNvrhiFormat(Candidate.format);
						return Format && *Format == m_NvrhiFormat && Candidate.colorSpace == m_SurfaceFormat.colorSpace;
					});
				if (CompatibleFormat == SurfaceFormats.end())
					throw std::runtime_error("The native window surface does not support the main editor's swapchain format");
				Candidate.SurfaceFormat = *CompatibleFormat;
				const auto SurfaceCapabilities = m_PhysicalDevice.getSurfaceCapabilitiesKHR(Candidate.Surface);
				const auto PresentModes = m_PhysicalDevice.getSurfacePresentModesKHR(Candidate.Surface);
				if (std::find(PresentModes.begin(), PresentModes.end(), vk::PresentModeKHR::eFifo) == PresentModes.end())
					throw std::runtime_error("The native window surface does not support FIFO presentation");
				Candidate.PresentMode = vk::PresentModeKHR::eFifo;
				if (!CreatePlatformSwapchain(Candidate, SurfaceCapabilities))
					throw std::runtime_error("The native editor window has no nonzero framebuffer extent");

				PF_CORE_INFO("Created Vulkan presentation target {} for a native editor window", TargetID);
#if defined(PF_DEBUG)
				if (m_DebugMainAcquireOutOfDateAfterNativeTarget)
				{
					m_DebugMainAcquireOutOfDateAfterNativeTarget = false;
					m_DebugMainAcquireOutOfDateAfterNativeTargetFrames = m_DebugMainAcquireOutOfDateAfterNativeTargetDelayFrames;
					PF_CORE_WARN(
						"Debug Vulkan presentation test will inject out-of-date acquisitions {} main frames after native target creation",
						m_DebugMainAcquireOutOfDateAfterNativeTargetFrames);
				}
#endif
				return TargetID;
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				std::string Message =
					std::string("Could not initialize Vulkan presentation for the native editor window: ") + Exception.what();
				try
				{
					ReleaseUnpublishedPlatformHost(Candidate);
					m_PlatformTargets.erase(HostEntry);
				}
				catch (const std::exception& CleanupException)
				{
					PF_CORE_ERROR("Could not release the unpublished Vulkan host; renderer retained ownership for shutdown: {}",
						CleanupException.what());
					Message += std::string("; cleanup remains owned by the renderer: ") + CleanupException.what();
				}
				catch (...)
				{
					PF_CORE_ERROR("Could not release the unpublished Vulkan host; renderer retained ownership for shutdown");
					Message += "; cleanup remains owned by the renderer after a non-standard cleanup exception";
				}
				return std::unexpected(std::move(Message));
			}
			catch (...)
			{
				std::string Message = "Could not initialize Vulkan presentation for the native editor window: non-standard exception";
				try
				{
					ReleaseUnpublishedPlatformHost(Candidate);
					m_PlatformTargets.erase(HostEntry);
				}
				catch (...)
				{
					PF_CORE_ERROR("Could not release the unpublished Vulkan host; renderer retained ownership for shutdown");
					Message += "; cleanup remains owned by the renderer";
				}
				return std::unexpected(std::move(Message));
			}
		}

		bool BeginPlatformPresentationTarget(uint64_t TargetID) override
		{
			m_DeviceHealth.EnsureOperational();
			const auto Entry = m_PlatformTargets.find(TargetID);
			if (Entry == m_PlatformTargets.end() || m_FrameActive || m_ActivePlatformTarget || m_ActiveRenderTarget)
				return false;
			PlatformPresentationHost& Host = *Entry->second;
			if (Host.RetirementBlocked)
				return false;
			Host.RetirePreviousGenerationsAfterSubmit = false;
			int FramebufferWidth = 0;
			int FramebufferHeight = 0;
			glfwGetFramebufferSize(Host.Window, &FramebufferWidth, &FramebufferHeight);
			if (FramebufferWidth <= 0 || FramebufferHeight <= 0)
				return false;
			if (Host.RecreateNeeded ||
				static_cast<uint32_t>(FramebufferWidth) != Host.Extent.width ||
				static_cast<uint32_t>(FramebufferHeight) != Host.Extent.height)
			{
				if (!RecreatePlatformSwapchain(Host, Host.SwapchainUnusable) &&
					(!Host.Swapchain || Host.RetirementBlocked || Host.SwapchainUnusable))
					return false;
			}

			FrameContext& Frame = Host.Frames[Host.CurrentFrame];
			WaitForFrame(Frame);
			uint32_t ImageIndex = 0;
			VkResult AcquireResult = VK_SUCCESS;
#if defined(PF_DEBUG)
			if (Host.InjectAcquireOutOfDate || Host.InjectAcquireOutOfDateCount > 0)
			{
				Host.InjectAcquireOutOfDate = false;
				if (Host.InjectAcquireOutOfDateCount > 0)
					--Host.InjectAcquireOutOfDateCount;
				Host.InjectedAcquireOutOfDatePendingRecovery = true;
				AcquireResult = VK_ERROR_OUT_OF_DATE_KHR;
				PF_CORE_INFO("Debug fixture injected VK_ERROR_OUT_OF_DATE_KHR for native host {}; no image was acquired and no command "
							 "list was opened",
					TargetID);
			}
			else
#endif
			{
				AcquireResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(
					static_cast<VkDevice>(m_Device),
					static_cast<VkSwapchainKHR>(Host.Swapchain),
					std::numeric_limits<uint64_t>::max(),
					static_cast<VkSemaphore>(Frame.ImageAvailable),
					VK_NULL_HANDLE,
					&ImageIndex);
			}
			if (AcquireResult == VK_ERROR_OUT_OF_DATE_KHR)
			{
				Host.RecreateNeeded = true;
				Host.SwapchainUnusable = true;
				return false;
			}
			if (AcquireResult != VK_SUCCESS && AcquireResult != VK_SUBOPTIMAL_KHR)
			{
				if (AcquireResult == VK_ERROR_DEVICE_LOST) MarkDeviceLost(AcquireResult);
				if (m_DeviceHealth.IsLost()) throw std::runtime_error("Vulkan device was lost while acquiring a native presentation image");
				PF_CORE_ERROR("Native editor window swapchain acquisition failed: {}",
					vk::to_string(static_cast<vk::Result>(AcquireResult)));
				Host.RecreateNeeded = true;
				return false;
			}
			if (ImageIndex >= Host.Images.size())
				throw std::runtime_error("Vulkan returned an invalid native-window swapchain image index");
#if defined(PF_DEBUG)
			if (Host.InjectedAcquireOutOfDatePendingRecovery)
			{
				Host.InjectedAcquireOutOfDatePendingRecovery = false;
				Host.InjectedAcquireOutOfDateRecoveryFramePending = true;
				PF_CORE_INFO("Native host {} recovered from the injected out-of-date acquire and acquired a swapchain image",
					TargetID);
			}
#endif

			Host.AcquiredImageIndex = ImageIndex;
			Host.AcquiredSuboptimal = AcquireResult == VK_SUBOPTIMAL_KHR;
			Host.RetirePreviousGenerationsAfterSubmit =
				!UsesPresentationFences() && Host.FirstPresentationPending && Host.FirstPresentationImageIndex == ImageIndex;
			SwapchainImage& Image = Host.Images[ImageIndex];
			m_PresentationResources.WaitForPresentation(Image);
			Frame.CommandList->open();
			const auto* DepthTarget = dynamic_cast<const VulkanTexture*>(Image.DepthTarget.get());
			if (!DepthTarget)
				throw std::runtime_error("Native editor window swapchain image has no compatible depth target");
			Frame.CommandList->clearTextureFloat(Image.Texture, nvrhi::TextureSubresourceSet(),
				nvrhi::Color(ClearColor[0], ClearColor[1], ClearColor[2], ClearColor[3]));
			Frame.CommandList->clearDepthStencilTexture(DepthTarget->GetNativeTexture(),
				nvrhi::TextureSubresourceSet(), true, 1.0f, false, 0);
			m_ActiveRenderTarget.reset();
			m_Recording = RecordingContext{
				&Frame, &Host.Images, Host.Extent, ImageIndex, Host.Swapchain, m_PresentQueue, false
			};
			m_ActivePlatformTargetID = TargetID;
			m_ActivePlatformTarget = &Host;
			return true;
		}

		void EndPlatformPresentationTarget(uint64_t TargetID) override
		{
			if (!m_ActivePlatformTarget || m_ActivePlatformTargetID != TargetID || !m_Recording.Frame)
				return;
			m_DeviceHealth.EnsureOperational();
			PlatformPresentationHost& Host = *m_ActivePlatformTarget;
			FrameContext& Frame = *m_Recording.Frame;
			SwapchainImage& Image = Host.Images[Host.AcquiredImageIndex];
			Frame.CommandList->setTextureState(Image.Texture, nvrhi::TextureSubresourceSet(), nvrhi::ResourceStates::Present);
			Frame.CommandList->commitBarriers();
			Frame.CommandList->close();
			m_VulkanDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics,
				static_cast<VkSemaphore>(Frame.ImageAvailable), 0);
			m_VulkanDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics,
				static_cast<VkSemaphore>(Image.PresentReady), 0);
			m_DeviceHealth.EnsureOperational();
			Frame.SubmissionValue =
				ExecuteNvrhiCommandList(Frame.CommandList, nvrhi::CommandQueue::Graphics, "submitting a native presentation frame");
			if (Host.RetirePreviousGenerationsAfterSubmit &&
				VulkanSupport::ShouldRetireDeferredSwapchains(UsesPresentationFences(), Host.FirstPresentationPending,
					Host.FirstPresentationImageIndex, Host.AcquiredImageIndex, true))
			{
				WaitForFrame(Frame);
				const size_t RetiredCount = Host.RetiredGenerations.size();
				ReleasePlatformRetiredGenerations(Host);
				PF_CORE_INFO("Native Vulkan presentation reacquire retired {} legacy swapchain generations",
					RetiredCount);
				Host.RetirePreviousGenerationsAfterSubmit = false;
			}
			m_PresentationResources.PrepareFenceForReuse(Image);
			const VkSemaphore PresentSemaphore = static_cast<VkSemaphore>(Image.PresentReady);
			const VkSwapchainKHR Swapchain = static_cast<VkSwapchainKHR>(Host.Swapchain);
			const VkFence NativePresentFence = static_cast<VkFence>(Image.PresentFence);
			VkSwapchainPresentFenceInfoKHR PresentFenceInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR, nullptr, 1,
				UsesPresentationFences() ? &NativePresentFence : nullptr };
			VkPresentInfoKHR PresentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, UsesPresentationFences() ? &PresentFenceInfo : nullptr, 1,
				&PresentSemaphore, 1, &Swapchain, &Host.AcquiredImageIndex, nullptr };
			const VkResult PresentResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(
				static_cast<VkQueue>(m_PresentQueue), &PresentInfo);
			if (PresentResult == VK_ERROR_DEVICE_LOST) MarkDeviceLost(PresentResult);
			const VulkanSupport::PresentationResultHandling PresentationHandling = RecordPresentationResult(Image, PresentResult);
			if (m_DeviceHealth.IsLost())
			{
				ClearActiveRecordingState();
				throw std::runtime_error("Vulkan device was lost while presenting a native swapchain image");
			}
			if (!UsesPresentationFences() && !Host.FirstPresentationPending && !Host.FirstPresentationCompleted &&
				(PresentResult == VK_SUCCESS || PresentResult == VK_SUBOPTIMAL_KHR))
			{
				Host.FirstPresentationPending = true;
				Host.FirstPresentationImageIndex = Host.AcquiredImageIndex;
			}
			Host.RecreateNeeded = Host.AcquiredSuboptimal ||
								  PresentationHandling.RecoveryAction == VulkanSupport::PresentationRecoveryAction::RecreateSwapchain;
			Host.SwapchainUnusable = !PresentationHandling.CurrentSwapchainUsable;
			if (PresentationHandling.RecoveryAction == VulkanSupport::PresentationRecoveryAction::BlockHost)
			{
				Host.RetirementBlocked = true;
				if (PresentationHandling.RequestDisposition == VulkanSupport::PresentationRequestDisposition::NotEnqueued)
				{
					PF_CORE_ERROR(
						"Native Vulkan presentation host {} stopped after vkQueuePresentKHR returned {} without enqueuing "
						"its semaphore wait. The host is blocked until close so the render-finished semaphore cannot be reused before "
						"its graphics signal operation completes",
						TargetID, vk::to_string(static_cast<vk::Result>(PresentResult)));
				}
				else
				{
					PF_CORE_ERROR("Native Vulkan presentation host {} stopped after vkQueuePresentKHR returned {}; request was {}. "
								  "The host will not be reused; its resources remain owned until the retirement wait succeeds",
						TargetID, vk::to_string(static_cast<vk::Result>(PresentResult)),
						VulkanSupport::ToString(PresentationHandling.RequestDisposition));
				}
			}
			if (PresentResult == VK_SUCCESS || PresentResult == VK_SUBOPTIMAL_KHR)
			{
				Host.RetirementBlocked = false;
				Host.RetirementBlockedLogged = false;
			}
#if defined(PF_DEBUG)
			if ((PresentResult == VK_SUCCESS || PresentResult == VK_SUBOPTIMAL_KHR) && m_SwapchainRetirementBlocked)
			{
				if (m_NativeFramesDuringMainRetirementBlock < 60)
					++m_NativeFramesDuringMainRetirementBlock;
				if (m_NativeFramesDuringMainRetirementBlock == 60 &&
					!m_LoggedNativePresentationDuringMainRetirementBlock)
				{
					m_LoggedNativePresentationDuringMainRetirementBlock = true;
					PF_CORE_INFO("Native Vulkan presentation remained active for 60 frames while the main swapchain was terminally blocked");
				}
			}
#endif
			if ((PresentResult == VK_SUCCESS || PresentResult == VK_SUBOPTIMAL_KHR) && !Host.LoggedFirstPresent)
			{
				Host.LoggedFirstPresent = true;
				PF_CORE_INFO("Native Vulkan presentation host submitted and presented its first frame");
			}
#if defined(PF_DEBUG)
			if (Host.InjectedAcquireOutOfDateRecoveryFramePending)
			{
				Host.InjectedAcquireOutOfDateRecoveryFramePending = false;
				if (PresentResult == VK_SUCCESS)
					PF_CORE_INFO("Native host {} submitted and presented a frame after out-of-date acquire recovery", TargetID);
				else
					PF_CORE_ERROR("Native host {} did not present its first frame after out-of-date acquire recovery: {}",
						TargetID, vk::to_string(static_cast<vk::Result>(PresentResult)));
			}
#endif
			if (PresentationHandling.RecoveryAction != VulkanSupport::PresentationRecoveryAction::BlockHost &&
				PresentResult != VK_SUCCESS && !Host.RecreateNeeded)
				PF_CORE_ERROR("Native editor window presentation failed: {}",
					vk::to_string(static_cast<vk::Result>(PresentResult)));
			RunNvrhiGarbageCollection();
			Host.CurrentFrame = (Host.CurrentFrame + 1) % Host.Frames.size();
			m_ActivePlatformTarget = nullptr;
			m_ActivePlatformTargetID = 0;
			m_Recording = m_FrameActive ? m_MainRecording : RecordingContext{};
		}

		bool DestroyPlatformPresentationTarget(uint64_t TargetID) override
		{
			if (m_DeviceHealth.IsLost())
			{
				PF_CORE_ERROR(
					"Native Vulkan host {} remains owned by the renderer after shared device loss; shutdown will retire all hosts together",
					TargetID);
				return false;
			}
			auto Entry = m_PlatformTargets.find(TargetID);
			if (Entry == m_PlatformTargets.end())
				return true;
			try
			{
				if (m_ActivePlatformTargetID == TargetID)
					EndPlatformPresentationTarget(TargetID);
				for (const FrameContext& Frame : Entry->second->Frames)
					WaitForFrame(Frame);
				m_PresentationResources.WaitForRetirement(Entry->second->Images, "native-window swapchain");
				for (RetiredSwapchainGeneration& Generation : Entry->second->RetiredGenerations)
					m_PresentationResources.WaitForRetirement(Generation.Images, "retired native-window swapchain");
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				PF_CORE_ERROR("Vulkan native-window retirement wait failed: {}", Exception.what());
				return false;
			}
			ReleasePlatformSwapchainImages(*Entry->second);
			ReleasePlatformRetiredGenerations(*Entry->second);
			ReleasePlatformFrames(*Entry->second);
			if (Entry->second->Swapchain)
				m_Device.destroySwapchainKHR(Entry->second->Swapchain);
			if (Entry->second->Surface)
				m_Instance.destroySurfaceKHR(Entry->second->Surface);
			m_PlatformTargets.erase(Entry);
			return true;
		}

		bool HasPlatformPresentationTargets() const noexcept override
		{
			return !m_PlatformTargets.empty();
		}

#if defined(PF_DEBUG)
		bool InjectNextPlatformAcquireOutOfDateForTesting(uint64_t TargetID) noexcept override
		{
			const auto Entry = m_PlatformTargets.find(TargetID);
			if (Entry == m_PlatformTargets.end() || Entry->second->InjectAcquireOutOfDate ||
				Entry->second->InjectAcquireOutOfDateCount != 0 ||
				Entry->second->InjectedAcquireOutOfDatePendingRecovery ||
				Entry->second->InjectedAcquireOutOfDateRecoveryFramePending)
				return false;
			Entry->second->InjectAcquireOutOfDateCount = 1;
			return true;
		}

		bool InjectNextPlatformImageAllocationFailureForTesting() noexcept override
		{
			if (m_InjectNextPlatformImageAllocationFailure)
				return false;
			m_InjectNextPlatformImageAllocationFailure = true;
			return true;
		}
#endif

		GraphicsResult BeginRenderTarget(
			const RenderTarget& Target,
			const RenderTargetClearValue& ClearValue) override
		{
			m_DeviceHealth.EnsureOperational();
			if (!m_Recording.Frame)
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
				FrameContext& Frame = *m_Recording.Frame;
				if (ColorTexture)
					Frame.CommandList->setTextureState(
						ColorTexture,
						nvrhi::TextureSubresourceSet(),
						nvrhi::ResourceStates::RenderTarget);
				if (DepthTexture)
					Frame.CommandList->setTextureState(
						DepthTexture,
						NativeTarget->GetDepthSubresources(),
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
						NativeTarget->GetDepthSubresources(),
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
					std::move(DepthTexture),
					NativeTarget->GetDepthSubresources()
				};
				return {};
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI render-target begin failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		GraphicsResult EndRenderTarget() override
		{
			m_DeviceHealth.EnsureOperational();
			if (!m_Recording.Frame || !m_ActiveRenderTarget)
			{
				return std::unexpected(GraphicsError{
					GraphicsErrorCode::InvalidFrameState,
					"EndRenderTarget requires an active frame and an active offscreen render target"
				});
			}

			try
			{
				FrameContext& Frame = *m_Recording.Frame;
				if (m_ActiveRenderTarget->ColorTexture)
					Frame.CommandList->setTextureState(
						m_ActiveRenderTarget->ColorTexture,
						nvrhi::TextureSubresourceSet(),
						nvrhi::ResourceStates::ShaderResource);
				if (m_ActiveRenderTarget->DepthShaderResource && m_ActiveRenderTarget->DepthTexture)
					Frame.CommandList->setTextureState(
						m_ActiveRenderTarget->DepthTexture,
						m_ActiveRenderTarget->DepthSubresources,
						nvrhi::ResourceStates::ShaderResource);
				Frame.CommandList->commitBarriers();
				m_ActiveRenderTarget.reset();
				return {};
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI render-target end failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		BufferCreateResult CreateBuffer(
			const BufferDesc& Description,
			std::span<const std::byte> InitialData) override
		{
			m_DeviceHealth.EnsureOperational();
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
					if (m_Recording.Frame)
					{
						m_Recording.Frame->CommandList->writeBuffer(NativeBuffer, InitialData.data(), InitialData.size());
					}
					else
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
						ExecuteNvrhiCommandList(UploadCommandList, nvrhi::CommandQueue::Graphics, "uploading a buffer");
					}
				}

				BufferHandle EngineBuffer = std::make_unique<VulkanBuffer>(Description, std::move(NativeBuffer));
				return BufferCreateResult(std::move(EngineBuffer));
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
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
			m_DeviceHealth.EnsureOperational();
			if (!m_Recording.Frame)
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
				m_Recording.Frame->CommandList->writeBuffer(
					NativeBuffer->GetNativeBuffer(),
					Data.data(),
					Data.size(),
					DestinationOffset);
				return {};
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
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
			m_DeviceHealth.EnsureOperational();
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
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI binding layout creation failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(BindingError{ BindingErrorCode::BackendFailure, Message });
			}
		}

		TextureCreateResult CreateTexture(
			const TextureDesc& Description,
			std::span<const TextureSubresourceData> InitialData) override
		{
			m_DeviceHealth.EnsureOperational();
			const auto Validation = ValidateTextureUpload(Description, InitialData);
			if (!Validation)
				return std::unexpected(Validation.error());

			nvrhi::Format Format = nvrhi::Format::UNKNOWN;
			switch (Description.Format)
			{
				case TextureFormat::RGBA8_UNorm: Format = nvrhi::Format::RGBA8_UNORM; break;
				case TextureFormat::RGBA8_Srgb: Format = nvrhi::Format::SRGBA8_UNORM; break;
				case TextureFormat::RGBA16_Float: Format = nvrhi::Format::RGBA16_FLOAT; break;
				case TextureFormat::RGBA32_Float: Format = nvrhi::Format::RGBA32_FLOAT; break;
				case TextureFormat::Depth32Float: Format = nvrhi::Format::D32; break;
				default: break;
			}
			if (Format == nvrhi::Format::UNKNOWN)
				return std::unexpected(TextureError{ TextureErrorCode::InvalidDescription, "Texture format has no Vulkan/NVRHI mapping" });

			const bool IsDepthAttachment = Description.Format == TextureFormat::Depth32Float;
			const bool IsColorAttachment = HasTextureUsage(Description.Usage, TextureUsage::ColorAttachment);
			const bool IsShaderResource = HasTextureUsage(Description.Usage, TextureUsage::ShaderResource);
			if (IsDepthAttachment)
			{
				const vk::FormatProperties Properties = m_PhysicalDevice.getFormatProperties(vk::Format::eD32Sfloat);
				const VkFormatFeatureFlags AvailableFeatures =
					static_cast<VkFormatFeatureFlags>(Properties.optimalTilingFeatures);
				if ((AvailableFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0 ||
					(IsShaderResource && (AvailableFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0))
					return std::unexpected(TextureError{ TextureErrorCode::UnsupportedFeature,
						"The active Vulkan device does not support the requested D32 depth attachment/sampling usage" });
			}
			if (Description.Format == TextureFormat::RGBA16_Float)
			{
				const vk::FormatProperties Properties = m_PhysicalDevice.getFormatProperties(vk::Format::eR16G16B16A16Sfloat);
				const VkFormatFeatureFlags AvailableFeatures =
					static_cast<VkFormatFeatureFlags>(Properties.optimalTilingFeatures);
				const VulkanSupport::FormatFeatureSupport Features{
					(AvailableFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
					(AvailableFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0,
					(AvailableFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0
				};
				if (!VulkanSupport::SupportsFormatUsage(Features, IsShaderResource, IsColorAttachment, false))
				{
					std::string RequiredUsage;
					if (IsShaderResource)
						RequiredUsage = "sampled-image";
					if (IsColorAttachment)
						RequiredUsage += RequiredUsage.empty() ? "color-attachment" : " and color-attachment";
					return std::unexpected(TextureError{
						TextureErrorCode::UnsupportedFeature,
						"The active Vulkan device does not support optimal-tiled RGBA16_Float " + RequiredUsage +
							" usage required by texture '" + Description.DebugName + "'"
					});
				}
			}
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

				const auto RecordTextureUpload = [&](auto& CommandList)
				{
					CommandList.setTextureState(
						NativeTexture,
						nvrhi::TextureSubresourceSet(),
						nvrhi::ResourceStates::CopyDest);
					CommandList.commitBarriers();
					for (const TextureSubresourceData& Subresource : InitialData)
					{
						const size_t BytesPerPixel = *GetTextureFormatBytesPerPixel(Description.Format);
						const size_t RowPitch = Subresource.RowPitch == 0
							? static_cast<size_t>(std::max(1u, Description.Width >> Subresource.MipLevel)) * BytesPerPixel
							: Subresource.RowPitch;
						CommandList.writeTexture(
							NativeTexture,
							Subresource.ArraySlice,
							Subresource.MipLevel,
							Subresource.Data.data(),
							RowPitch,
							0);
					}
					CommandList.setTextureState(
						NativeTexture,
						nvrhi::TextureSubresourceSet(),
						IsShaderResource ? nvrhi::ResourceStates::ShaderResource : nvrhi::ResourceStates::RenderTarget);
					CommandList.commitBarriers();
				};

				if (m_Recording.Frame)
				{
					RecordTextureUpload(*m_Recording.Frame->CommandList);
				}
				else
				{
					nvrhi::CommandListHandle UploadCommandList = m_ActiveNvrhiDevice->createCommandList();
					if (!UploadCommandList)
					{
						const std::string Message = "NVRHI failed to create a texture upload command list";
						PF_CORE_ERROR("{0}", Message);
						return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
					}

					UploadCommandList->open();
					RecordTextureUpload(*UploadCommandList);
					UploadCommandList->close();
					ExecuteNvrhiCommandList(UploadCommandList, nvrhi::CommandQueue::Graphics, "uploading a texture");
				}

				PF_CORE_INFO("Uploaded Vulkan/NVRHI {0}x{1} texture '{2}'",
					Description.Width,
					Description.Height,
					Description.DebugName);
				return TextureHandle(std::make_unique<VulkanTexture>(Description, std::move(NativeTexture)));
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI texture creation/upload failed for '" +
					Description.DebugName + "': " + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
			}
		}

		TextureReadbackResult ReadTextureForValidation(const Texture& Source, uint32_t ArraySlice) override
		{
			m_DeviceHealth.EnsureOperational();
			if (m_Recording.Frame || m_ActiveRenderTarget)
			{
				return std::unexpected(TextureError{
					TextureErrorCode::InvalidData,
					"Validation texture readback requires an idle renderer frame"
				});
			}

			const TextureDesc& Description = Source.GetDescription();
			const bool IsColor = Description.Format == TextureFormat::RGBA8_UNorm || Description.Format == TextureFormat::RGBA8_Srgb ||
				Description.Format == TextureFormat::RGBA16_Float;
			const bool IsDepth = Description.Format == TextureFormat::Depth32Float;
			const nvrhi::ResourceStates RestoreState = HasTextureUsage(Description.Usage, TextureUsage::ShaderResource)
				? nvrhi::ResourceStates::ShaderResource
				: IsDepth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
			const uint32_t SliceCount = Description.Dimension == TextureDimension::TextureCube ? 6u : 1u;
			if (Description.MipLevels != 1 || (!IsColor && !IsDepth) || ArraySlice >= SliceCount)
			{
				return std::unexpected(TextureError{
					TextureErrorCode::UnsupportedFeature,
					"Validation readback supports one RGBA8, RGBA16F, or D32 slice from a single-mip 2D texture or cubemap"
				});
			}

			const auto* NativeSource = dynamic_cast<const VulkanTexture*>(&Source);
			if (!NativeSource || !m_ActiveNvrhiDevice)
			{
				return std::unexpected(TextureError{
					TextureErrorCode::UnsupportedFeature,
					"Validation texture readback requires a texture from the active Vulkan renderer"
				});
			}

			try
			{
				WaitForNvrhiIdleForOperation("waiting for the rendered source texture");

				nvrhi::TextureDesc StagingDescription = NativeSource->GetNativeTexture()->getDesc();
				StagingDescription
					.setDimension(nvrhi::TextureDimension::Texture2D)
					.setArraySize(1)
					.setMipLevels(1)
					.setIsRenderTarget(false)
					.setInitialState(nvrhi::ResourceStates::CopyDest)
					.setKeepInitialState(true)
					.setDebugName("PulseForge validation readback staging texture");
				StagingDescription.isShaderResource = false;
				nvrhi::StagingTextureHandle Staging = m_ActiveNvrhiDevice->createStagingTexture(
					StagingDescription,
					nvrhi::CpuAccessMode::Read);
				if (!Staging)
					throw std::runtime_error("NVRHI could not create a color/depth readback staging texture");

				nvrhi::CommandListHandle CopyCommands = m_ActiveNvrhiDevice->createCommandList();
				if (!CopyCommands)
					throw std::runtime_error("NVRHI could not create a readback command list");
				CopyCommands->open();
				const nvrhi::TextureSubresourceSet SourceSubresources(0, 1, ArraySlice, 1);
				nvrhi::TextureSlice SourceSlice;
				SourceSlice.arraySlice = ArraySlice;
				CopyCommands->setTextureState(
					NativeSource->GetNativeTexture(),
					SourceSubresources,
					nvrhi::ResourceStates::CopySource);
				CopyCommands->commitBarriers();
				CopyCommands->copyTexture(Staging, nvrhi::TextureSlice(), NativeSource->GetNativeTexture(), SourceSlice);
				CopyCommands->setTextureState(
					NativeSource->GetNativeTexture(),
					SourceSubresources,
					RestoreState);
				CopyCommands->commitBarriers();
				CopyCommands->close();
				ExecuteNvrhiCommandList(CopyCommands, nvrhi::CommandQueue::Graphics, "submitting a texture readback copy");
				WaitForNvrhiIdleForOperation("waiting for the texture readback copy");

				size_t RowPitch = 0;
				const nvrhi::TextureSlice Slice;
				const void* Mapped = m_ActiveNvrhiDevice->mapStagingTexture(
					Staging,
					Slice,
					nvrhi::CpuAccessMode::Read,
					&RowPitch);
				if (!Mapped)
					throw std::runtime_error("NVRHI returned no mapped data for the requested validation readback");
				struct StagingUnmap
				{
					nvrhi::IDevice* Device;
					nvrhi::IStagingTexture* Texture;
					~StagingUnmap() { Device->unmapStagingTexture(Texture); }
				} Unmap{ m_ActiveNvrhiDevice, Staging };

				TextureReadback Result;
				Result.Width = Description.Width;
				Result.Height = Description.Height;
				Result.Format = Description.Format;
				const size_t BytesPerPixel = IsDepth
					? sizeof(float)
					: *GetTextureFormatBytesPerPixel(Description.Format);
				if (Description.Width > (std::numeric_limits<size_t>::max)() / BytesPerPixel)
					throw std::runtime_error("Validation readback row size overflowed");
				const size_t PackedRowSize = static_cast<size_t>(Description.Width) * BytesPerPixel;
				if (RowPitch < PackedRowSize) throw std::runtime_error("NVRHI returned an invalid color/depth readback row pitch");
				if (Description.Height > (std::numeric_limits<size_t>::max)() / PackedRowSize)
					throw std::runtime_error("Validation readback image size overflowed");
				if (Description.Height > 1 && RowPitch > ((std::numeric_limits<size_t>::max)() - PackedRowSize) / (Description.Height - 1))
					throw std::runtime_error("Validation readback source row offsets overflowed");
				Result.Pixels.resize(PackedRowSize * Description.Height);
				const auto* SourceBytes = static_cast<const std::byte*>(Mapped);
				for (uint32_t Row = 0; Row < Description.Height; ++Row)
					std::memcpy(
						Result.Pixels.data() + static_cast<size_t>(Row) * PackedRowSize,
						SourceBytes + static_cast<size_t>(Row) * RowPitch,
						PackedRowSize);
				return Result;
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI validation texture readback failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
			}
		}

		RenderTargetCreateResult CreateRenderTarget(const RenderTargetDesc& Description) override
		{
			m_DeviceHealth.EnsureOperational();
			const auto Validation = ValidateRenderTargetDescription(Description);
			if (!Validation)
				return std::unexpected(Validation.error());

			TextureCreateResult ColorTexture = TextureHandle{};
			if (Description.ColorFormat != ColorTargetFormat::None)
			{
				TextureDesc ColorDescription;
				ColorDescription.Width = Description.Width;
				ColorDescription.Height = Description.Height;
				switch (Description.ColorFormat)
				{
					case ColorTargetFormat::RGBA8_UNorm: ColorDescription.Format = TextureFormat::RGBA8_UNorm; break;
					case ColorTargetFormat::RGBA8_Srgb: ColorDescription.Format = TextureFormat::RGBA8_Srgb; break;
					case ColorTargetFormat::RGBA16_Float: ColorDescription.Format = TextureFormat::RGBA16_Float; break;
					default:
						return std::unexpected(RenderTargetError{
							RenderTargetErrorCode::InvalidDescription,
							"Offscreen color target format has no texture representation"
						});
				}
				ColorDescription.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
				ColorDescription.DebugName = Description.DebugName.empty()
					? "PulseForge offscreen color"
					: Description.DebugName + " color";
				ColorTexture = CreateTexture(ColorDescription, {});
				if (!ColorTexture)
				{
					const RenderTargetErrorCode Code = ColorTexture.error().Code == TextureErrorCode::UnsupportedFeature
						? RenderTargetErrorCode::UnsupportedFeature
						: ColorTexture.error().Code == TextureErrorCode::InvalidDescription
							? RenderTargetErrorCode::InvalidDescription
							: RenderTargetErrorCode::BackendFailure;
					return std::unexpected(RenderTargetError{ Code,
						"Could not create render-target color texture: " + ColorTexture.error().Message });
				}
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
						DepthTexture.error().Code == TextureErrorCode::UnsupportedFeature
							? RenderTargetErrorCode::UnsupportedFeature
							: RenderTargetErrorCode::BackendFailure,
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
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI render-target creation failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(RenderTargetError{ RenderTargetErrorCode::BackendFailure, Message });
			}
		}

		DepthCubemapCreateResult CreateDepthCubemap(uint32_t Resolution, const std::string& DebugName) override
		{
			m_DeviceHealth.EnsureOperational();
			if (Resolution == 0 || DebugName.empty())
				return std::unexpected(DepthCubemapError{ DepthCubemapErrorCode::InvalidDescription,
					"Depth cubemap resolution and debug name must be valid" });

			TextureDesc TextureDescription;
			TextureDescription.Width = Resolution;
			TextureDescription.Height = Resolution;
			TextureDescription.Format = TextureFormat::Depth32Float;
			TextureDescription.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource;
			TextureDescription.Dimension = TextureDimension::TextureCube;
			TextureDescription.DebugName = DebugName;
			auto TextureResult = CreateTexture(TextureDescription, std::span<const TextureSubresourceData>{});
			if (!TextureResult)
				return std::unexpected(DepthCubemapError{
					TextureResult.error().Code == TextureErrorCode::UnsupportedFeature
						? DepthCubemapErrorCode::UnsupportedFeature
						: DepthCubemapErrorCode::BackendFailure,
					"Could not create shader-readable D32 cubemap: " + TextureResult.error().Message });

			const auto* NativeTexture = dynamic_cast<const VulkanTexture*>(TextureResult->get());
			if (!NativeTexture)
				return std::unexpected(DepthCubemapError{ DepthCubemapErrorCode::UnsupportedFeature,
					"The depth cubemap texture was not created by the active Vulkan renderer" });

			auto Cubemap = std::make_unique<VulkanDepthCubemap>(std::move(*TextureResult), Resolution);
			for (uint32_t Face = 0; Face < 6; ++Face)
			{
				RenderTargetDesc FaceDescription;
				FaceDescription.Width = Resolution;
				FaceDescription.Height = Resolution;
				FaceDescription.ColorFormat = ColorTargetFormat::None;
				FaceDescription.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
				FaceDescription.DebugName = DebugName + " face " + std::to_string(Face);
				nvrhi::FramebufferDesc FramebufferDescription;
				FramebufferDescription.setDepthAttachment(NativeTexture->GetNativeTexture(),
					nvrhi::TextureSubresourceSet(0, 1, Face, 1));
				try
				{
					nvrhi::FramebufferHandle Framebuffer = m_ActiveNvrhiDevice->createFramebuffer(FramebufferDescription);
					if (!Framebuffer)
						return std::unexpected(DepthCubemapError{ DepthCubemapErrorCode::BackendFailure,
							"NVRHI failed to create a depth cubemap face framebuffer" });
					Cubemap->SetFaceTarget(Face, std::make_unique<VulkanRenderTarget>(
						FaceDescription, TextureHandle{}, TextureHandle{}, std::move(Framebuffer),
						Cubemap ? &Cubemap->GetTexture() : nullptr, Face));
				}
				catch (const std::exception& Exception)
				{
					MarkDeviceLostFromException(Exception);
					return std::unexpected(DepthCubemapError{ DepthCubemapErrorCode::BackendFailure,
						"NVRHI failed to create a depth cubemap face framebuffer: " + std::string(Exception.what()) });
				}
			}
			return DepthCubemapHandle(std::move(Cubemap));
		}

		SamplerCreateResult CreateSampler(const SamplerDesc& Description) override
		{
			m_DeviceHealth.EnsureOperational();
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
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI sampler creation failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(TextureError{ TextureErrorCode::BackendFailure, Message });
			}
		}

		BindingSetCreateResult CreateBindingSet(const BindingSetDesc& Description) override
		{
			m_DeviceHealth.EnsureOperational();
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
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI binding-set creation failed: " + std::string(Exception.what());
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(BindingError{ BindingErrorCode::BackendFailure, Message });
			}
		}

		ShaderCreateResult CreateShader(
			const ShaderDesc& Description,
			std::span<const std::byte> Bytecode) override
		{
			m_DeviceHealth.EnsureOperational();
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
				MarkDeviceLostFromException(Exception);
				const std::string Message = "Vulkan/NVRHI shader creation failed for '" + Description.DebugName + "': " + Exception.what();
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(GraphicsError{ GraphicsErrorCode::BackendFailure, Message });
			}
		}

		GraphicsPipelineCreateResult CreateGraphicsPipeline(const GraphicsPipelineDesc& Description) override
		{
			m_DeviceHealth.EnsureOperational();
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
			if (Description.Rasterizer.Wireframe && !m_FillModeNonSolidEnabled)
			{
				const std::string Message = "Vulkan device does not support the fillModeNonSolid feature required by wireframe pipeline '" +
					Description.DebugName + "'";
				return std::unexpected(GraphicsError{ GraphicsErrorCode::UnsupportedFeature, Message });
			}
			if (Description.VertexLayout.Stride != 0 &&
				Description.VertexLayout.Stride % m_MinVertexInputBindingStrideAlignment != 0)
			{
				const std::string Message = "Vulkan device requires vertex stride alignment of " +
					std::to_string(m_MinVertexInputBindingStrideAlignment) + " bytes for pipeline '" + Description.DebugName + "'";
				return std::unexpected(GraphicsError{ GraphicsErrorCode::UnsupportedFeature, Message });
			}
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
					case VertexSemantic::Tangent: SemanticName = "TANGENT"; break;
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
				case CullMode::None:
					Rasterizer.setCullNone();
					break;
				case CullMode::Back:
					Rasterizer.setCullBack();
					break;
				case CullMode::Front:
					Rasterizer.setCullFront();
					break;
				}
				Rasterizer.setFrontCounterClockwise(Description.Rasterizer.FrontCounterClockwise)
					.setFillMode(Description.Rasterizer.Wireframe ? nvrhi::RasterFillMode::Wireframe : nvrhi::RasterFillMode::Solid)
					.setScissorEnable(Description.Rasterizer.ScissorEnabled)
					.setDepthBias(static_cast<int32_t>(std::lround(Description.Rasterizer.DepthBias)))
					.setSlopeScaleDepthBias(Description.Rasterizer.SlopeScaledDepthBias);

				nvrhi::ComparisonFunc DepthComparison = nvrhi::ComparisonFunc::Less;
				switch (Description.Depth.Compare)
				{
				case DepthCompareOperation::Never:
					DepthComparison = nvrhi::ComparisonFunc::Never;
					break;
				case DepthCompareOperation::Less:
					DepthComparison = nvrhi::ComparisonFunc::Less;
					break;
				case DepthCompareOperation::Equal:
					DepthComparison = nvrhi::ComparisonFunc::Equal;
					break;
				case DepthCompareOperation::LessEqual:
					DepthComparison = nvrhi::ComparisonFunc::LessOrEqual;
					break;
				case DepthCompareOperation::Greater:
					DepthComparison = nvrhi::ComparisonFunc::Greater;
					break;
				case DepthCompareOperation::NotEqual:
					DepthComparison = nvrhi::ComparisonFunc::NotEqual;
					break;
				case DepthCompareOperation::GreaterEqual:
					DepthComparison = nvrhi::ComparisonFunc::GreaterOrEqual;
					break;
				case DepthCompareOperation::Always:
					DepthComparison = nvrhi::ComparisonFunc::Always;
					break;
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
				{
					nvrhi::Format ColorFormat = nvrhi::Format::UNKNOWN;
					switch (Description.ColorFormat)
					{
						case ColorTargetFormat::Swapchain: ColorFormat = m_NvrhiFormat; break;
						case ColorTargetFormat::RGBA8_UNorm: ColorFormat = nvrhi::Format::RGBA8_UNORM; break;
						case ColorTargetFormat::RGBA8_Srgb: ColorFormat = nvrhi::Format::SRGBA8_UNORM; break;
						case ColorTargetFormat::RGBA16_Float: ColorFormat = nvrhi::Format::RGBA16_FLOAT; break;
						default: break;
					}
					if (ColorFormat == nvrhi::Format::UNKNOWN)
						return std::unexpected(GraphicsError{
							GraphicsErrorCode::InvalidDescription,
							"Graphics pipeline color format has no Vulkan/NVRHI framebuffer mapping"
						});
					FramebufferInfo.addColorFormat(ColorFormat);
				}
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
				MarkDeviceLostFromException(Exception);
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
				FrameContext& Frame = *m_Recording.Frame;
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
				MarkDeviceLostFromException(Exception);
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
				FrameContext& Frame = *m_Recording.Frame;
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
				MarkDeviceLostFromException(Exception);
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
			m_DeviceHealth.EnsureOperational();
			OutSkipDraw = false;
			if (!m_Recording.Frame)
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
					if (!m_Recording.Images || m_Recording.ImageIndex >= m_Recording.Images->size())
						return std::unexpected(GraphicsError{
							GraphicsErrorCode::InvalidFrameState,
							"The active Vulkan frame has no acquired presentation image"
						});
					SwapchainImage& Image = (*m_Recording.Images)[m_Recording.ImageIndex];
					TargetWidth = m_Recording.Extent.width;
					TargetHeight = m_Recording.Extent.height;
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

				m_Recording.Frame->CommandList->setGraphicsState(State);
				return {};
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
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
			nvrhi::TextureSubresourceSet DepthSubresources;
		};

		using SwapchainImage = VulkanSwapchainImage;
		using RetiredSwapchainGeneration = VulkanRetiredSwapchainGeneration;

		struct RecordingContext
		{
			FrameContext* Frame = nullptr;
			std::vector<SwapchainImage>* Images = nullptr;
			vk::Extent2D Extent;
			uint32_t ImageIndex = 0;
			vk::SwapchainKHR Swapchain;
			vk::Queue PresentQueue;
			bool IsMain = false;
		};

		struct PlatformPresentationHost
		{
			GLFWwindow* Window = nullptr;
			vk::SurfaceKHR Surface;
			vk::SwapchainKHR Swapchain;
			vk::Extent2D Extent;
			vk::SurfaceFormatKHR SurfaceFormat;
			vk::PresentModeKHR PresentMode = vk::PresentModeKHR::eFifo;
			std::vector<SwapchainImage> Images;
			std::vector<RetiredSwapchainGeneration> RetiredGenerations;
			std::array<FrameContext, FramesInFlight> Frames;
			uint32_t CurrentFrame = 0;
			uint32_t AcquiredImageIndex = 0;
			uint32_t FirstPresentationImageIndex = 0;
			bool RecreateNeeded = false;
			bool AcquiredSuboptimal = false;
			bool FirstPresentationPending = false;
			bool FirstPresentationCompleted = false;
			bool DeferredRetirementLimitLogged = false;
			bool SwapchainUnusable = false;
			bool RetirementBlocked = false;
			bool RetirementBlockedLogged = false;
			bool LegacyRecoveryAttemptedAtLimit = false;
			bool LoggedFirstPresent = false;
			bool RetirePreviousGenerationsAfterSubmit = false;
#if defined(PF_DEBUG)
			bool InjectAcquireOutOfDate = false;
			uint32_t InjectAcquireOutOfDateCount = 0;
			bool InjectedAcquireOutOfDatePendingRecovery = false;
			bool InjectedAcquireOutOfDateRecoveryFramePending = false;
			bool InjectImageAllocationFailure = false;
			uint32_t InjectSwapchainCreationFailureAtDeferredCount = 0;
			bool InjectSwapchainCreationFailureConsumed = false;
#endif
		};

		void Initialize()
		{
			if (!m_NativeWindow)
				throw std::runtime_error("Vulkan renderer requires a valid GLFW window");

			InitializeVulkanLoader();
			if (glfwVulkanSupported() != GLFW_TRUE)
				throw std::runtime_error("GLFW could not find a usable Vulkan loader and installable driver");

			ConfigurePresentationPolicyAndDebugValidation();
			CreateInstance();
			CreateSurface();
			SelectPhysicalDevice();
			CreateLogicalDevice();
			CreateNvrhiDevice();
			m_PresentationResources.Initialize(
				m_Device, m_PresentQueue, m_ActiveNvrhiDevice, m_PresentationRetirementMechanism, &m_DeviceHealth);
			CreateFrameContexts();
			(void)RecreateSwapchain();
		}

		void ConfigurePresentationPolicyAndDebugValidation()
		{
			const char* PolicyValue = std::getenv("PULSEFORGE_VULKAN_PRESENTATION_POLICY");
			const std::string PolicyText = PolicyValue ? PolicyValue : "";
			const auto Policy = VulkanSupport::ParsePresentationPolicy(PolicyText);
			if (!Policy)
				throw std::runtime_error(
					"Invalid PULSEFORGE_VULKAN_PRESENTATION_POLICY value '" + PolicyText + "'. Use 'auto' (default) or 'require-fences'.");
			m_PresentationPolicy = *Policy;

#if defined(PF_DEBUG)
			const auto IsEnabled = [](const char* Name)
			{
				const char* Value = std::getenv(Name);
				return Value && std::strcmp(Value, "1") == 0;
			};
			m_ForceLegacyRetirementForTesting = IsEnabled("PULSEFORGE_VULKAN_FORCE_LEGACY_RETIREMENT");
			if (m_PresentationPolicy == VulkanSupport::PresentationPolicy::RequireFences && m_ForceLegacyRetirementForTesting)
				throw std::runtime_error("PULSEFORGE_VULKAN_PRESENTATION_POLICY=require-fences cannot be combined with the Debug-only "
										 "PULSEFORGE_VULKAN_FORCE_LEGACY_RETIREMENT test override");
			m_DebugMainAcquireOutOfDateAfterNativeTarget =
				IsEnabled("PULSEFORGE_VULKAN_TEST_MAIN_OUT_OF_DATE_AFTER_NATIVE_TARGET");
			const auto ReadCount = [](const char* Name, unsigned long Maximum = 16ul)
			{
				const char* Value = std::getenv(Name);
				if (!Value || !*Value)
					return uint32_t{};
				char* End = nullptr;
				const unsigned long Parsed = std::strtoul(Value, &End, 10);
				return End != Value && End && *End == '\0' && Parsed <= Maximum ? static_cast<uint32_t>(Parsed) : uint32_t{};
			};
			m_DebugMainAcquireOutOfDateRemaining = ReadCount("PULSEFORGE_VULKAN_TEST_MAIN_ACQUIRE_OUT_OF_DATE_COUNT");
			m_DebugNativeAcquireOutOfDateCount = ReadCount("PULSEFORGE_VULKAN_TEST_NATIVE_ACQUIRE_OUT_OF_DATE_COUNT");
			m_DebugMainAcquireOutOfDateAfterNativeTargetCount =
				ReadCount("PULSEFORGE_VULKAN_TEST_MAIN_ACQUIRE_OUT_OF_DATE_AFTER_NATIVE_TARGET_COUNT");
			m_DebugMainAcquireOutOfDateAfterNativeTargetDelayFrames =
				ReadCount("PULSEFORGE_VULKAN_TEST_MAIN_ACQUIRE_OUT_OF_DATE_AFTER_NATIVE_TARGET_DELAY_FRAMES", 600ul);
			if (!m_DebugMainAcquireOutOfDateAfterNativeTargetDelayFrames) m_DebugMainAcquireOutOfDateAfterNativeTargetDelayFrames = 60;
			if (m_DebugMainAcquireOutOfDateAfterNativeTarget && !m_DebugMainAcquireOutOfDateAfterNativeTargetCount)
				m_DebugMainAcquireOutOfDateAfterNativeTargetCount = static_cast<uint32_t>(
					VulkanSupport::MaximumDeferredSwapchainGenerations + VulkanSupport::MaximumOutOfDateRecoveryGenerations + 1);
			m_DebugMainSwapchainCreationFailureAtDeferredCount = ReadCount("PULSEFORGE_VULKAN_TEST_MAIN_CREATE_FAILURE_AT_DEFERRED_COUNT");
			m_DebugNativeSwapchainCreationFailureAtDeferredCount =
				ReadCount("PULSEFORGE_VULKAN_TEST_NATIVE_CREATE_FAILURE_AT_DEFERRED_COUNT");
			if (m_ForceLegacyRetirementForTesting || m_DebugMainAcquireOutOfDateRemaining || m_DebugMainAcquireOutOfDateAfterNativeTarget ||
				m_DebugNativeAcquireOutOfDateCount || m_DebugMainSwapchainCreationFailureAtDeferredCount ||
				m_DebugNativeSwapchainCreationFailureAtDeferredCount)
				PF_CORE_WARN(
					"Debug Vulkan presentation test enabled: legacy retirement forced={}, main acquire injections={}, main-after-native "
					"injection count={}, first native host acquire injections={}, main/native create-failure limits={}/{}",
					m_ForceLegacyRetirementForTesting, m_DebugMainAcquireOutOfDateRemaining,
					m_DebugMainAcquireOutOfDateAfterNativeTargetCount, m_DebugNativeAcquireOutOfDateCount,
					m_DebugMainSwapchainCreationFailureAtDeferredCount, m_DebugNativeSwapchainCreationFailureAtDeferredCount);
#endif
			PF_CORE_INFO("Vulkan presentation support policy: {}", VulkanSupport::ToString(m_PresentationPolicy));
		}

		[[noreturn]] static void FailStopUnsafeShutdown(const ShutdownResult& Result) noexcept
		{
			if (Result.Disposition == ShutdownDisposition::PresentationLifetimeUnknown)
			{
				PF_CORE_CRITICAL(
					"Vulkan device loss was confirmed, but a maintenance presentation fence did not signal; the presentation engine's "
					"resource-reference handoff is unknown (wait result {}). The renderer cannot safely destroy the swapchain children",
					Result.VulkanResult);
			}
			else if (Result.Disposition == ShutdownDisposition::WaitFailed)
			{
				PF_CORE_CRITICAL(
					"Vulkan shutdown could not complete wait stage {} (wait outcome {}, VkResult {}). The device is not confirmed lost and "
					"the renderer cannot safely destroy resources whose completion remains unknown",
					static_cast<uint32_t>(Result.Stage), static_cast<uint32_t>(Result.WaitOutcome), Result.VulkanResult);
			}
			else
			{
				PF_CORE_CRITICAL(
					"Vulkan shutdown failed while releasing stage {} (disposition {}). Vulkan children may still be owned; terminating "
					"rather than continuing with partial teardown",
					static_cast<uint32_t>(Result.Stage), static_cast<uint32_t>(Result.Disposition));
			}
			std::terminate();
		}

		void MarkDeviceLost(VkResult Result) noexcept
		{
			if (Result == VK_ERROR_DEVICE_LOST) m_DeviceHealth.MarkLost(Result);
		}

		void MarkDeviceLostFromException(const std::exception& Exception) noexcept
		{
			if (dynamic_cast<const vk::DeviceLostError*>(&Exception)) MarkDeviceLost(VK_ERROR_DEVICE_LOST);
		}

		void CheckVkResult(VkResult Result, const char* Operation)
		{
			MarkDeviceLost(Result);
			if (Result != VK_SUCCESS)
				throw std::runtime_error(
					std::string(Operation) + " failed with Vulkan result " + vk::to_string(static_cast<vk::Result>(Result)));
		}

		ShutdownWaitResult ClassifyShutdownVkResult(VkResult Result) noexcept
		{
			if (Result == VK_SUCCESS) return {};
			if (Result == VK_ERROR_DEVICE_LOST)
			{
				MarkDeviceLost(Result);
				return { ShutdownWaitOutcome::DeviceLost, Result };
			}
			return { ShutdownWaitOutcome::Failed, Result };
		}

		void RunNvrhiGarbageCollection()
		{
			if (!m_ActiveNvrhiDevice || m_DeviceHealth.IsLost()) return;
			try
			{
				m_ActiveNvrhiDevice->runGarbageCollection();
				if (m_DeviceHealth.IsLost())
				{
					ClearActiveRecordingState();
					throw std::runtime_error("NVRHI reported Vulkan device loss during timeline-semaphore collection");
				}
			}
			catch (const vk::DeviceLostError&)
			{
				MarkDeviceLost(VK_ERROR_DEVICE_LOST);
				ClearActiveRecordingState();
				throw std::runtime_error("NVRHI timeline-semaphore collection reported Vulkan device loss");
			}
		}

		uint64_t ExecuteNvrhiCommandList(nvrhi::ICommandList* CommandList, nvrhi::CommandQueue Queue, const char* Operation)
		{
			m_DeviceHealth.EnsureOperational();
			if (!m_ActiveNvrhiDevice) throw std::runtime_error("The Vulkan renderer has no active NVRHI device");

			uint64_t SubmissionValue = 0;
			try
			{
				SubmissionValue = m_ActiveNvrhiDevice->executeCommandList(CommandList, Queue);
			}
			catch (const vk::DeviceLostError&)
			{
				MarkDeviceLost(VK_ERROR_DEVICE_LOST);
				ClearActiveRecordingState();
				throw std::runtime_error(std::string("NVRHI reported Vulkan device loss while ") + Operation);
			}

			if (m_DeviceHealth.IsLost())
			{
				ClearActiveRecordingState();
				throw std::runtime_error(std::string("NVRHI reported Vulkan device loss while ") + Operation);
			}
			return SubmissionValue;
		}

		void WaitForNvrhiIdleForOperation(const char* Operation)
		{
			m_DeviceHealth.EnsureOperational();
			if (!m_ActiveNvrhiDevice) throw std::runtime_error("The Vulkan renderer has no active NVRHI device");
			try
			{
				if (!m_ActiveNvrhiDevice->waitForIdle())
				{
					MarkDeviceLost(VK_ERROR_DEVICE_LOST);
					throw std::runtime_error(std::string("NVRHI reported Vulkan device loss while ") + Operation);
				}
				m_DeviceHealth.EnsureOperational();
			}
			catch (const vk::DeviceLostError&)
			{
				MarkDeviceLost(VK_ERROR_DEVICE_LOST);
				throw std::runtime_error(std::string("NVRHI reported Vulkan device loss while ") + Operation);
			}
		}

		void ClearActiveRecordingState() noexcept
		{
			m_FrameActive = false;
			m_Recording = {};
			m_MainRecording = {};
			m_ActivePlatformTarget = nullptr;
			m_ActivePlatformTargetID = 0;
		}

		[[nodiscard]] bool UsesPresentationFences() const noexcept
		{
			return m_PresentationRetirementMechanism != VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort;
		}

		void InitializeVulkanLoader()
		{
#if defined(__APPLE__)
			const auto GLFWProcAddress = GLFWWindow::GetVulkanLoaderProcAddress();
			if (!GLFWProcAddress)
				throw std::runtime_error("GLFW did not retain the Vulkan loader selected before initialization");
			const auto GetInstanceProcAddress = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GLFWProcAddress);
#endif
#if !defined(__APPLE__)
			if (!m_VulkanLoader) m_VulkanLoader = std::make_unique<vk::detail::DynamicLoader>();
			const auto GetInstanceProcAddress = m_VulkanLoader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
#endif
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
				throw std::runtime_error("GLFW did not report the Vulkan surface extensions required by its selected display backend; "
										 "check that the Vulkan loader/ICD and WSI integration are available for this display session");

			for (uint32_t Index = 0; Index < RequiredExtensionCount; ++Index)
			{
				if (!HasName(AvailableExtensions, RequiredExtensions[Index]))
					throw std::runtime_error(std::string("The Vulkan loader is missing GLFW-required instance extension ") + RequiredExtensions[Index]);
				AppendUniqueName(m_EnabledInstanceExtensions, RequiredExtensions[Index]);
			}

			m_PortabilityEnumerationEnabled = HasName(AvailableExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			if (m_PortabilityEnumerationEnabled)
			{
				AppendUniqueName(m_EnabledInstanceExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
				PF_CORE_INFO("Vulkan portability enumeration enabled");
			}

			m_DebugUtilsEnabled = HasName(AvailableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			if (m_DebugUtilsEnabled) AppendUniqueName(m_EnabledInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

			m_AvailablePresentationInstanceExtensions = { HasName(AvailableExtensions, VK_KHR_SURFACE_EXTENSION_NAME),
				HasName(AvailableExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME),
				HasName(AvailableExtensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME),
				HasName(AvailableExtensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) };
			m_EnabledPresentationInstanceExtensions =
				VulkanSupport::PlanPresentationInstanceExtensions(m_AvailablePresentationInstanceExtensions);
			if (m_EnabledPresentationInstanceExtensions.SurfaceCapabilities2)
				AppendUniqueName(m_EnabledInstanceExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
			if (m_EnabledPresentationInstanceExtensions.KhrSurfaceMaintenance)
				AppendUniqueName(m_EnabledInstanceExtensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
			if (m_EnabledPresentationInstanceExtensions.ExtSurfaceMaintenance)
				AppendUniqueName(m_EnabledInstanceExtensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
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
			if (m_PortabilityEnumerationEnabled)
				CreateInfo.setFlags(vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR);
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
			Candidate.PortabilitySubsetExtensionAvailable =
				HasName(Extensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
			Candidate.AvailablePresentationDeviceExtensions = { HasName(Extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
				HasName(Extensions, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME),
				HasName(Extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) };
			if (!Candidate.AvailablePresentationDeviceExtensions.Swapchain) return std::nullopt;

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
			vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR SupportedSwapchainMaintenance;
			vk::PhysicalDevicePortabilitySubsetFeaturesKHR SupportedPortabilitySubset;
			const bool HasCompatibleMaintenancePath = VulkanSupport::HasCompatiblePresentationFencePath(
				m_EnabledPresentationInstanceExtensions, Candidate.AvailablePresentationDeviceExtensions);
			Supported12.setPNext(&Supported13);
			if (Candidate.PortabilitySubsetExtensionAvailable)
			{
				Supported13.setPNext(&SupportedPortabilitySubset);
				if (HasCompatibleMaintenancePath)
					SupportedPortabilitySubset.setPNext(&SupportedSwapchainMaintenance);
			}
			else if (HasCompatibleMaintenancePath)
			{
				Supported13.setPNext(&SupportedSwapchainMaintenance);
			}
			vk::PhysicalDeviceFeatures2 SupportedFeatures;
			SupportedFeatures.setPNext(&Supported12);
			PhysicalDevice.getFeatures2(&SupportedFeatures);
			Candidate.FillModeNonSolidSupported = SupportedFeatures.features.fillModeNonSolid == VK_TRUE;
			if (Candidate.PortabilitySubsetExtensionAvailable)
			{
				vk::PhysicalDeviceProperties2 Properties2;
				vk::PhysicalDevicePortabilitySubsetPropertiesKHR PortabilityProperties;
				Properties2.pNext = &PortabilityProperties;
				PhysicalDevice.getProperties2(&Properties2);
				Candidate.MinVertexInputBindingStrideAlignment = PortabilityProperties.minVertexInputBindingStrideAlignment;
				if (Candidate.MinVertexInputBindingStrideAlignment == 0)
					return std::nullopt;
			}
			const VulkanSupport::RequiredDeviceFeatures RequiredFeatures{
				Supported12.timelineSemaphore == VK_TRUE,
				Supported13.synchronization2 == VK_TRUE,
				Supported13.dynamicRendering == VK_TRUE,
				Supported13.shaderDemoteToHelperInvocation == VK_TRUE
			};
			if (!VulkanSupport::SupportsRequiredDeviceFeatures(RequiredFeatures))
				return std::nullopt;

			Candidate.QueueFamilies = *QueueSelection;
			Candidate.SurfaceFormat = SurfaceFormat->first;
			Candidate.NvrhiFormat = SurfaceFormat->second;
			Candidate.PresentMode = ToVkPresentMode(*PresentMode);
			Candidate.PresentationFeatureSupported =
				HasCompatibleMaintenancePath && SupportedSwapchainMaintenance.swapchainMaintenance1 == VK_TRUE;
			const VulkanSupport::PresentationFenceCapability FenceCapability = VulkanSupport::EvaluatePresentationFenceCapability(
				m_AvailablePresentationInstanceExtensions, m_EnabledPresentationInstanceExtensions,
				Candidate.AvailablePresentationDeviceExtensions, Candidate.PresentationFeatureSupported);
			const VulkanSupport::PresentationPolicySelection PresentationSelection =
				VulkanSupport::ResolvePresentationPolicy(m_PresentationPolicy, FenceCapability, m_ForceLegacyRetirementForTesting);
			if (!PresentationSelection.Eligible)
			{
				if (!m_StrictPresentationRejectionReason) m_StrictPresentationRejectionReason = PresentationSelection.Reason;
				return std::nullopt;
			}
			Candidate.PresentationRetirementMechanism = PresentationSelection.Mechanism;
			Candidate.PresentationCapabilityReason = PresentationSelection.Reason;
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
			m_StrictPresentationRejectionReason.reset();
			for (vk::PhysicalDevice PhysicalDevice : PhysicalDevices)
			{
				auto Candidate = EvaluateDevice(PhysicalDevice);
				if (Candidate && (!BestCandidate || VulkanSupport::IsBetterPresentationCandidate(
														Candidate->PresentationRetirementMechanism !=
															VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort,
														Candidate->Score,
														BestCandidate->PresentationRetirementMechanism !=
															VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort,
														BestCandidate->Score)))
					BestCandidate = std::move(Candidate);
			}

			if (!BestCandidate)
			{
				if (m_PresentationPolicy == VulkanSupport::PresentationPolicy::RequireFences && m_StrictPresentationRejectionReason)
					throw std::runtime_error(
						std::string("No otherwise suitable Vulkan GPU satisfies PULSEFORGE_VULKAN_PRESENTATION_POLICY=require-fences: ") +
						VulkanSupport::ToString(*m_StrictPresentationRejectionReason) +
						". Use PULSEFORGE_VULKAN_PRESENTATION_POLICY=auto to allow the documented legacy compatibility path.");
				throw std::runtime_error(
					"No Vulkan 1.3 GPU supports graphics/presentation, VK_KHR_swapchain, "
					"timeline semaphores, synchronization2, dynamic rendering, D32 depth attachments, "
					"a recognized swapchain format, and color/transfer-destination images");
			}

			m_PhysicalDevice = BestCandidate->Device;
			m_QueueFamilies = BestCandidate->QueueFamilies;
			m_SurfaceFormat = BestCandidate->SurfaceFormat;
			m_NvrhiFormat = BestCandidate->NvrhiFormat;
			m_MinVertexInputBindingStrideAlignment = BestCandidate->MinVertexInputBindingStrideAlignment;
			m_FillModeNonSolidEnabled = BestCandidate->FillModeNonSolidSupported;
			m_PortabilitySubsetEnabled = BestCandidate->PortabilitySubsetExtensionAvailable;
			m_PresentMode = BestCandidate->PresentMode;
			m_PresentationRetirementMechanism = BestCandidate->PresentationRetirementMechanism;
			m_SwapchainMaintenanceFeatureEnabled = BestCandidate->PresentationFeatureSupported && UsesPresentationFences();
			if (m_SwapchainMaintenanceFeatureEnabled != UsesPresentationFences())
				throw std::logic_error("Selected Vulkan presentation path does not match the queried swapchainMaintenance1 feature");
			m_EnabledDeviceExtensions.clear();
			AppendUniqueName(m_EnabledDeviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
			if (BestCandidate->PortabilitySubsetExtensionAvailable)
			{
				AppendUniqueName(m_EnabledDeviceExtensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
				PF_CORE_INFO("Vulkan portability subset enabled for selected physical device");
			}
			switch (m_PresentationRetirementMechanism)
			{
			case VulkanSupport::PresentationRetirementMechanism::KhrMaintenanceFence:
				AppendUniqueName(m_EnabledDeviceExtensions, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
				break;
			case VulkanSupport::PresentationRetirementMechanism::ExtMaintenanceFence:
				AppendUniqueName(m_EnabledDeviceExtensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
				break;
			case VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort:
				break;
			}

			PF_CORE_INFO(
				"Selected Vulkan GPU: {0} (API {1}, driver {2}, graphics queue {3}, present queue {4})",
				std::string(BestCandidate->Properties.deviceName.data()),
				ApiVersionString(BestCandidate->Properties.apiVersion),
				BestCandidate->Properties.driverVersion,
				m_QueueFamilies.GraphicsIndex,
				m_QueueFamilies.PresentIndex);
			if (UsesPresentationFences())
			{
				PF_CORE_INFO("Vulkan presentation retirement: {} (swapchainMaintenance1 feature enabled, policy: {})",
					VulkanSupport::ToString(m_PresentationRetirementMechanism), VulkanSupport::ToString(m_PresentationPolicy));
			}
			else
			{
				PF_CORE_WARN(
					"Vulkan presentation retirement: {} (policy: {}). Reason: {}. Final close/shutdown uses a best-effort "
					"queue-idle fallback; Vulkan does not provide an explicit presentation-engine release guarantee on this legacy path.",
					VulkanSupport::ToString(m_PresentationRetirementMechanism), VulkanSupport::ToString(m_PresentationPolicy),
					VulkanSupport::ToString(BestCandidate->PresentationCapabilityReason));
			}
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

			vk::PhysicalDeviceFeatures2 EnabledFeatures;
			EnabledFeatures.features.setFillModeNonSolid(m_FillModeNonSolidEnabled ? VK_TRUE : VK_FALSE);
			vk::PhysicalDeviceVulkan12Features Enabled12;
			Enabled12.setTimelineSemaphore(VK_TRUE);
			vk::PhysicalDeviceVulkan13Features Enabled13;
			vk::PhysicalDevicePortabilitySubsetFeaturesKHR EnabledPortabilitySubset;
			vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR EnabledSwapchainMaintenance;
			// No optional portability-subset feature is requested by the renderer's current pipeline contract.
			Enabled13
				.setSynchronization2(VK_TRUE)
				.setDynamicRendering(VK_TRUE)
				.setShaderDemoteToHelperInvocation(VK_TRUE);
			if (m_SwapchainMaintenanceFeatureEnabled)
			{
				EnabledSwapchainMaintenance.setSwapchainMaintenance1(VK_TRUE);
				if (m_PortabilitySubsetEnabled)
					EnabledPortabilitySubset.setPNext(&EnabledSwapchainMaintenance);
				else
					Enabled13.setPNext(&EnabledSwapchainMaintenance);
			}
			if (m_PortabilitySubsetEnabled)
				Enabled13.setPNext(&EnabledPortabilitySubset);
			Enabled12.setPNext(&Enabled13);

			vk::DeviceCreateInfo CreateInfo;
			EnabledFeatures.setPNext(&Enabled12);
			CreateInfo.setPNext(&EnabledFeatures).setQueueCreateInfos(QueueCreateInfos).setPEnabledExtensionNames(m_EnabledDeviceExtensions);

			m_Device = m_PhysicalDevice.createDevice(CreateInfo);
			VULKAN_HPP_DEFAULT_DISPATCHER.init(m_Device);
			m_GraphicsQueue = m_Device.getQueue(m_QueueFamilies.GraphicsIndex, 0);
			m_PresentQueue = m_Device.getQueue(m_QueueFamilies.PresentIndex, 0);
		}

		void CreateNvrhiDevice()
		{
			nvrhi::vulkan::DeviceDesc Description;
			Description.errorCB = &m_NvrhiMessageCallback;
			Description.instance = static_cast<VkInstance>(m_Instance);
			Description.physicalDevice = static_cast<VkPhysicalDevice>(m_PhysicalDevice);
			Description.device = static_cast<VkDevice>(m_Device);
			Description.graphicsQueue = static_cast<VkQueue>(m_GraphicsQueue);
			Description.graphicsQueueIndex = static_cast<int>(m_QueueFamilies.GraphicsIndex);
			Description.deviceExtensions = m_EnabledDeviceExtensions.data();
			Description.numDeviceExtensions = m_EnabledDeviceExtensions.size();
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
			m_DeviceHealth.EnsureOperational();
			if (Frame.SubmissionValue == 0)
				return;

			const vk::Semaphore CompletionSemaphore(m_VulkanDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics));
			const VkSemaphore NativeCompletionSemaphore = static_cast<VkSemaphore>(CompletionSemaphore);
			const std::array<uint64_t, 1> Values = { Frame.SubmissionValue };
			const VkSemaphoreWaitInfo WaitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &NativeCompletionSemaphore,
				Values.data() };
			const VkResult WaitResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(
				static_cast<VkDevice>(m_Device), &WaitInfo, std::numeric_limits<uint64_t>::max());
			if (WaitResult != VK_SUCCESS)
			{
				MarkDeviceLost(WaitResult);
				throw std::runtime_error("Vulkan frame synchronization wait failed: " + vk::to_string(static_cast<vk::Result>(WaitResult)));
			}
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

		void ReleaseSwapchainImages() { m_PresentationResources.ReleaseImages(m_SwapchainImages); }

		void RetireMainSwapchainsAfterReacquire()
		{
			m_PresentationResources.ReleaseGenerations(m_RetiredSwapchainGenerations);
			m_FirstPresentationPending = false;
			m_FirstPresentationCompleted = true;
			m_DeferredRetirementLimitLogged = false;
			m_LegacyRecoveryAttemptedAtLimit = false;
		}

		bool RecreateSwapchain(bool OutOfDateRecovery = false)
		{
			if (!m_Device)
				return false;
			if (!VulkanSupport::CanCreateSwapchainGeneration(UsesPresentationFences(), m_Swapchain != vk::SwapchainKHR(),
					m_RetiredSwapchainGenerations.size(), OutOfDateRecovery, m_LegacyRecoveryAttemptedAtLimit))
			{
				if (!m_DeferredRetirementLimitLogged)
				{
					PF_CORE_ERROR(
						"Main swapchain recreation is deferred because {} legacy presentation generations are still awaiting reacquisition",
						m_RetiredSwapchainGenerations.size());
					m_DeferredRetirementLimitLogged = true;
				}
				return false;
			}

			const auto [FramebufferWidth, FramebufferHeight] = m_Window.GetFramebufferSize();
			if (FramebufferWidth == 0 || FramebufferHeight == 0)
				return false;

			const auto Capabilities = m_PhysicalDevice.getSurfaceCapabilitiesKHR(m_Surface);
			auto Extent = ChooseSwapchainExtent(Capabilities);
			if (!Extent)
				return false;

			for (const FrameContext& Frame : m_Frames)
				WaitForFrame(Frame);
			if (UsesPresentationFences()) m_PresentationResources.WaitForRetirement(m_SwapchainImages, "main swapchain");
			else if (m_Swapchain)
			{
				try
				{
					m_RetiredSwapchainGenerations.reserve(m_RetiredSwapchainGenerations.size() + 1);
				}
				catch (const std::exception& Exception)
				{
					MarkDeviceLostFromException(Exception);
					PF_CORE_ERROR("Could not reserve bounded legacy swapchain retirement storage: {}", Exception.what());
					return false;
				}
			}

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
				.setOldSwapchain(m_Swapchain)
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

			vk::SwapchainKHR NewSwapchain;
			std::vector<SwapchainImage> NewImages;
			const bool HadPreviousGeneration = m_Swapchain != vk::SwapchainKHR();
			bool PreviousGenerationRetired = false;
			const bool RecoveryAtLegacyLimit = !UsesPresentationFences() &&
											   m_RetiredSwapchainGenerations.size() == VulkanSupport::MaximumDeferredSwapchainGenerations &&
											   (m_SwapchainUnusable || !HadPreviousGeneration);
			try
			{
				if (RecoveryAtLegacyLimit) m_LegacyRecoveryAttemptedAtLimit = true;
#if defined(PF_DEBUG)
				if (RecoveryAtLegacyLimit && m_DebugMainSwapchainCreationFailureAtDeferredCount > 0 &&
					!m_DebugMainSwapchainCreationFailureConsumed &&
					m_DebugMainSwapchainCreationFailureAtDeferredCount == m_RetiredSwapchainGenerations.size())
				{
					m_DebugMainSwapchainCreationFailureConsumed = true;
					throw std::runtime_error("Debug fixture injected a main swapchain creation failure at the legacy recovery limit");
				}
#endif
				NewSwapchain = m_Device.createSwapchainKHR(PresentInfo);
				if (m_Swapchain)
				{
					// A successful create with oldSwapchain retires the old generation. With
					// presentation fences, its completion is known directly. Otherwise retain it
					// until the first new-chain present is reacquired.
					if (UsesPresentationFences())
					{
						ReleaseSwapchainImages();
						m_Device.destroySwapchainKHR(m_Swapchain);
					}
					else
						m_RetiredSwapchainGenerations.push_back({ m_Swapchain, std::move(m_SwapchainImages) });
					m_Swapchain = vk::SwapchainKHR();
					m_SwapchainExtent = vk::Extent2D();
					PreviousGenerationRetired = true;
					m_FirstPresentationPending = false;
					m_FirstPresentationCompleted = false;
				}
				const auto NativeImages = m_Device.getSwapchainImagesKHR(NewSwapchain);
				if (NativeImages.empty())
					throw std::runtime_error("Vulkan created a swapchain without any images");
				NewImages.reserve(NativeImages.size());
				for (size_t Index = 0; Index < NativeImages.size(); ++Index)
				{
					NewImages.emplace_back();
					SwapchainImage& Image = NewImages.back();
					Image.NativeImage = NativeImages[Index];
					Image.PresentReady = m_Device.createSemaphore(vk::SemaphoreCreateInfo());
					if (UsesPresentationFences()) Image.PresentFence = m_Device.createFence(vk::FenceCreateInfo());

					nvrhi::TextureDesc TextureDescription;
					TextureDescription
						.setDimension(nvrhi::TextureDimension::Texture2D)
						.setFormat(m_NvrhiFormat)
						.setWidth(Extent->width)
						.setHeight(Extent->height)
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
					DepthDescription.Width = Extent->width;
					DepthDescription.Height = Extent->height;
					DepthDescription.Format = TextureFormat::Depth32Float;
					DepthDescription.Usage = TextureUsage::DepthStencilAttachment;
					DepthDescription.DebugName = "PulseForge swapchain depth image " + std::to_string(Index);
					auto CreatedDepthTarget = CreateTexture(DepthDescription, std::span<const TextureSubresourceData>{});
					if (!CreatedDepthTarget)
						throw std::runtime_error("Could not create a Vulkan swapchain depth target: " +
							CreatedDepthTarget.error().Message);
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
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				m_PresentationResources.ReleaseImages(NewImages, PresentationResourceReleaseMode::UnpresentedCandidate);
				if (NewSwapchain)
					m_Device.destroySwapchainKHR(NewSwapchain);
				m_SwapchainRecreationNeeded = true;
				if (HadPreviousGeneration && !PreviousGenerationRetired)
					PF_CORE_ERROR("Could not prepare replacement Vulkan swapchain; the previous generation remains available: {0}",
						Exception.what());
				else if (PreviousGenerationRetired)
					PF_CORE_ERROR("Replacement Vulkan swapchain failed after the previous generation was retired; the main surface will be "
								  "retried: {0}",
						Exception.what());
				else
					PF_CORE_ERROR("Could not create the initial Vulkan swapchain: {0}", Exception.what());
				return false;
			}

			ReleaseSwapchainImages();
			if (m_Swapchain)
				m_Device.destroySwapchainKHR(m_Swapchain);
			m_Swapchain = NewSwapchain;
			m_SwapchainExtent = *Extent;
			m_SwapchainImages = std::move(NewImages);

			m_SwapchainRecreationNeeded = false;
			m_SwapchainUnusable = false;
			m_LegacyRecoveryAttemptedAtLimit = false;
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

		bool CreatePlatformSwapchain(
			PlatformPresentationHost& Host,
			const vk::SurfaceCapabilitiesKHR& Capabilities,
			bool CreateFrameContexts = true,
			vk::SwapchainKHR OldSwapchain = {})
		{
			int FramebufferWidth = 0;
			int FramebufferHeight = 0;
			glfwGetFramebufferSize(Host.Window, &FramebufferWidth, &FramebufferHeight);
			if (FramebufferWidth <= 0 || FramebufferHeight <= 0)
				return false;

			const bool HasFixedExtent = Capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max();
			const VulkanSupport::Extent2D ChosenExtent = VulkanSupport::ChooseExtent(
				{ static_cast<uint32_t>(FramebufferWidth), static_cast<uint32_t>(FramebufferHeight) },
				{ Capabilities.currentExtent.width, Capabilities.currentExtent.height },
				HasFixedExtent,
				{ Capabilities.minImageExtent.width, Capabilities.minImageExtent.height },
				{ Capabilities.maxImageExtent.width, Capabilities.maxImageExtent.height });
			if (ChosenExtent.Width == 0 || ChosenExtent.Height == 0)
				return false;
			Host.Extent = vk::Extent2D(ChosenExtent.Width, ChosenExtent.Height);

			const std::array<uint32_t, 2> QueueIndices = {
				m_QueueFamilies.GraphicsIndex, m_QueueFamilies.PresentIndex
			};
			const bool SeparateQueues = QueueIndices[0] != QueueIndices[1];
			const vk::ImageUsageFlags ImageUsage =
				vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
			const uint32_t ImageCount = VulkanSupport::ChooseImageCount(
				Capabilities.minImageCount, Capabilities.maxImageCount);
			vk::SwapchainCreateInfoKHR CreateInfo;
			CreateInfo
				.setSurface(Host.Surface)
				.setOldSwapchain(OldSwapchain)
				.setMinImageCount(ImageCount)
				.setImageFormat(Host.SurfaceFormat.format)
				.setImageColorSpace(Host.SurfaceFormat.colorSpace)
				.setImageExtent(Host.Extent)
				.setImageArrayLayers(1)
				.setImageUsage(ImageUsage)
				.setImageSharingMode(SeparateQueues ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive)
				.setPreTransform(Capabilities.currentTransform)
				.setCompositeAlpha(ChooseCompositeAlpha(Capabilities.supportedCompositeAlpha))
				.setPresentMode(Host.PresentMode)
				.setClipped(VK_TRUE);
			if (SeparateQueues)
				CreateInfo.setQueueFamilyIndices(QueueIndices);

			Host.Swapchain = m_Device.createSwapchainKHR(CreateInfo);
			const std::vector<vk::Image> NativeImages = m_Device.getSwapchainImagesKHR(Host.Swapchain);
			if (NativeImages.empty())
				throw std::runtime_error("Vulkan created a native-window swapchain without images");
			Host.Images.reserve(NativeImages.size());
			for (size_t Index = 0; Index < NativeImages.size(); ++Index)
			{
				Host.Images.emplace_back();
				SwapchainImage& Image = Host.Images.back();
				Image.NativeImage = NativeImages[Index];
				Image.PresentReady = m_Device.createSemaphore(vk::SemaphoreCreateInfo());
				if (UsesPresentationFences()) Image.PresentFence = m_Device.createFence(vk::FenceCreateInfo());

				nvrhi::TextureDesc TextureDescription;
				TextureDescription
					.setDimension(nvrhi::TextureDimension::Texture2D)
					.setFormat(m_NvrhiFormat)
					.setWidth(Host.Extent.width)
					.setHeight(Host.Extent.height)
					.setIsRenderTarget(true)
					.enableAutomaticStateTracking(nvrhi::ResourceStates::Present)
					.setDebugName("PulseForge native editor swapchain image " + std::to_string(Index));
				Image.Texture = m_ActiveNvrhiDevice->createHandleForNativeTexture(
					nvrhi::ObjectTypes::VK_Image,
					nvrhi::Object(ToNativeHandleValue(static_cast<VkImage>(Image.NativeImage))),
					TextureDescription);
				if (!Image.Texture)
					throw std::runtime_error("NVRHI could not wrap a native editor swapchain image");

				TextureDesc DepthDescription;
				DepthDescription.Width = Host.Extent.width;
				DepthDescription.Height = Host.Extent.height;
				DepthDescription.Format = TextureFormat::Depth32Float;
				DepthDescription.Usage = TextureUsage::DepthStencilAttachment;
				DepthDescription.DebugName = "PulseForge native editor swapchain depth image " + std::to_string(Index);
				auto Depth = CreateTexture(DepthDescription, std::span<const TextureSubresourceData>{});
				if (!Depth)
					throw std::runtime_error("Could not create native editor swapchain depth: " + Depth.error().Message);
				Image.DepthTarget = std::move(*Depth);
				const auto* NativeDepth = dynamic_cast<const VulkanTexture*>(Image.DepthTarget.get());
				if (!NativeDepth)
					throw std::runtime_error("Native editor swapchain depth texture has an incompatible backend");
#if defined(PF_DEBUG)
				if (Index == 0 && Host.InjectImageAllocationFailure)
				{
					Host.InjectImageAllocationFailure = false;
					PF_CORE_WARN("Debug fixture injected a native presentation allocation failure after creating swapchain image {} and "
								 "its depth target",
						Index);
					throw std::runtime_error(
						"Debug fixture injected allocation failure after native swapchain image and depth-target creation");
				}
#endif
				nvrhi::FramebufferDesc FramebufferDescription;
				FramebufferDescription.addColorAttachment(Image.Texture);
				FramebufferDescription.setDepthAttachment(NativeDepth->GetNativeTexture());
				Image.Framebuffer = m_ActiveNvrhiDevice->createFramebuffer(FramebufferDescription);
				if (!Image.Framebuffer)
					throw std::runtime_error("NVRHI could not create a native editor swapchain framebuffer");
			}

			if (CreateFrameContexts && !Host.Frames[0].CommandList)
			{
				for (FrameContext& Frame : Host.Frames)
				{
					Frame.ImageAvailable = m_Device.createSemaphore(vk::SemaphoreCreateInfo());
					Frame.CommandList = m_ActiveNvrhiDevice->createCommandList();
					if (!Frame.CommandList)
						throw std::runtime_error("NVRHI could not create a native-window frame command list");
				}
			}
		Host.RecreateNeeded = false;
		return true;
		}

		void ReleasePlatformSwapchainImages(PlatformPresentationHost& Host)
		{
			ReleasePlatformSwapchainImages(Host.Images);
		}

		void ReleasePlatformSwapchainImages(std::vector<SwapchainImage>& Images) { m_PresentationResources.ReleaseImages(Images); }

		void ReleasePlatformRetiredGenerations(PlatformPresentationHost& Host)
		{
			m_PresentationResources.ReleaseGenerations(Host.RetiredGenerations);
			Host.FirstPresentationPending = false;
			Host.FirstPresentationCompleted = true;
			Host.DeferredRetirementLimitLogged = false;
			Host.LegacyRecoveryAttemptedAtLimit = false;
		}

		void ReleasePlatformFrames(PlatformPresentationHost& Host)
		{
			for (FrameContext& Frame : Host.Frames)
			{
				Frame.CommandList = nullptr;
				Frame.SubmissionValue = 0;
				if (Frame.ImageAvailable && m_Device)
					m_Device.destroySemaphore(Frame.ImageAvailable);
				Frame.ImageAvailable = vk::Semaphore();
			}
		}

		void ReleaseUnpublishedPlatformHost(PlatformPresentationHost& Host)
		{
			constexpr auto ReleaseMode = PresentationResourceReleaseMode::UnpresentedCandidate;
			m_PresentationResources.ReleaseImages(Host.Images, ReleaseMode);
			m_PresentationResources.ReleaseGenerations(Host.RetiredGenerations, ReleaseMode);
			ReleasePlatformFrames(Host);
			if (Host.Swapchain) m_Device.destroySwapchainKHR(Host.Swapchain);
			Host.Swapchain = vk::SwapchainKHR();
			if (m_Instance && Host.Surface) m_Instance.destroySurfaceKHR(Host.Surface);
			Host.Surface = vk::SurfaceKHR();
		}

		bool RecreatePlatformSwapchain(PlatformPresentationHost& Host, bool OutOfDateRecovery = false)
		{
			if (!VulkanSupport::CanCreateSwapchainGeneration(UsesPresentationFences(), Host.Swapchain != vk::SwapchainKHR(),
					Host.RetiredGenerations.size(), OutOfDateRecovery, Host.LegacyRecoveryAttemptedAtLimit))
			{
				if ((Host.SwapchainUnusable || !Host.Swapchain) &&
					(Host.RetiredGenerations.size() > VulkanSupport::MaximumDeferredSwapchainGenerations ||
						(Host.RetiredGenerations.size() == VulkanSupport::MaximumDeferredSwapchainGenerations &&
							Host.LegacyRecoveryAttemptedAtLimit)))
				{
					Host.RetirementBlocked = true;
					if (!Host.RetirementBlockedLogged)
					{
						PF_CORE_ERROR("A native Vulkan presentation host is terminally blocked: its current swapchain is out of date and "
									  "{} retired generations still await a specification-backed reacquire completion; the document "
									  "remains open and this host requires recreation after renderer restart",
							Host.RetiredGenerations.size());
						Host.RetirementBlockedLogged = true;
					}
				}
				if (!Host.DeferredRetirementLimitLogged)
				{
					PF_CORE_ERROR("Native swapchain recreation is deferred because {} legacy presentation generations are still awaiting "
								  "reacquisition",
						Host.RetiredGenerations.size());
					Host.DeferredRetirementLimitLogged = true;
				}
				return false;
			}

			PlatformPresentationHost Replacement;
			Replacement.Window = Host.Window;
			Replacement.Surface = Host.Surface;
			Replacement.SurfaceFormat = Host.SurfaceFormat;
			Replacement.PresentMode = Host.PresentMode;
			bool PreviousGenerationRetired = false;
			try
			{
				for (const FrameContext& Frame : Host.Frames)
					WaitForFrame(Frame);
				if (UsesPresentationFences()) m_PresentationResources.WaitForRetirement(Host.Images, "resized native-window swapchain");
				else if (Host.Swapchain)
					Host.RetiredGenerations.reserve(Host.RetiredGenerations.size() + 1);
				const auto Capabilities = m_PhysicalDevice.getSurfaceCapabilitiesKHR(Host.Surface);
				const bool RecoveryAtLegacyLimit = !UsesPresentationFences() &&
												   Host.RetiredGenerations.size() == VulkanSupport::MaximumDeferredSwapchainGenerations &&
												   (Host.SwapchainUnusable || !Host.Swapchain);
				if (RecoveryAtLegacyLimit) Host.LegacyRecoveryAttemptedAtLimit = true;
#if defined(PF_DEBUG)
				if (RecoveryAtLegacyLimit && Host.InjectSwapchainCreationFailureAtDeferredCount > 0 &&
					!Host.InjectSwapchainCreationFailureConsumed &&
					Host.InjectSwapchainCreationFailureAtDeferredCount == Host.RetiredGenerations.size())
				{
					Host.InjectSwapchainCreationFailureConsumed = true;
					Host.RecreateNeeded = true;
					PF_CORE_WARN("Debug fixture injected a native swapchain creation failure at the legacy recovery limit");
					return false;
				}
#endif
				if (!CreatePlatformSwapchain(Replacement, Capabilities, false, Host.Swapchain))
				{
					m_PresentationResources.ReleaseImages(Replacement.Images, PresentationResourceReleaseMode::UnpresentedCandidate);
					m_PresentationResources.ReleaseGenerations(
						Replacement.RetiredGenerations, PresentationResourceReleaseMode::UnpresentedCandidate);
					if (Replacement.Swapchain)
						m_Device.destroySwapchainKHR(Replacement.Swapchain);
					Host.RecreateNeeded = true;
					return false;
				}
				if (Host.Swapchain)
				{
					const vk::SwapchainKHR PreviousSwapchain = Host.Swapchain;
					if (UsesPresentationFences())
					{
						ReleasePlatformSwapchainImages(Host);
						m_Device.destroySwapchainKHR(PreviousSwapchain);
					}
					else Host.RetiredGenerations.push_back({ PreviousSwapchain, std::move(Host.Images) });
					Host.Swapchain = vk::SwapchainKHR();
					Host.Extent = vk::Extent2D();
					PreviousGenerationRetired = true;
				}
				Host.Swapchain = Replacement.Swapchain;
				Replacement.Swapchain = vk::SwapchainKHR();
				Host.Extent = Replacement.Extent;
				Host.Images = std::move(Replacement.Images);
				Host.FirstPresentationPending = false;
				Host.FirstPresentationCompleted = false;
				Host.DeferredRetirementLimitLogged = false;
				Host.RecreateNeeded = false;
				Host.SwapchainUnusable = false;
				Host.LegacyRecoveryAttemptedAtLimit = false;
				return true;
			}
			catch (const std::exception& Exception)
			{
				MarkDeviceLostFromException(Exception);
				PreviousGenerationRetired = PreviousGenerationRetired || (Host.Swapchain && Replacement.Swapchain);
				m_PresentationResources.ReleaseImages(Replacement.Images, PresentationResourceReleaseMode::UnpresentedCandidate);
				m_PresentationResources.ReleaseGenerations(
					Replacement.RetiredGenerations, PresentationResourceReleaseMode::UnpresentedCandidate);
				if (Replacement.Swapchain) m_Device.destroySwapchainKHR(Replacement.Swapchain);
				if (m_DeviceHealth.IsLost())
				{
					// Keep the previously published host and its presentation state for coordinated shutdown.
					Host.RetirementBlocked = true;
					Host.RecreateNeeded = false;
					PF_CORE_ERROR(
						"Vulkan device loss interrupted native swapchain recreation; the host remains owned for renderer shutdown: {}",
						Exception.what());
				}
				else if (PreviousGenerationRetired)
				{
					if (UsesPresentationFences() && Host.Swapchain)
					{
						ReleasePlatformSwapchainImages(Host);
						m_Device.destroySwapchainKHR(Host.Swapchain);
					}
					else if (Host.Swapchain) Host.RetiredGenerations.push_back({ Host.Swapchain, std::move(Host.Images) });
					Host.Swapchain = vk::SwapchainKHR();
					Host.Extent = vk::Extent2D();
					PF_CORE_ERROR(
						"Native swapchain replacement failed after the prior generation was retired; the surface will be retried: {}",
						Exception.what());
				}
				else
					PF_CORE_ERROR("Could not recreate native editor window swapchain: {}", Exception.what());
				Host.RecreateNeeded = true;
				return false;
			}
		}

		ShutdownResult Shutdown() noexcept
		{
			ClearActiveRecordingState();
			const ShutdownResult Result = ShutdownRenderer(*this);
			if (Result.Disposition == ShutdownDisposition::CompleteAfterDeviceLoss)
			{
				if (UsesPresentationFences())
					PF_CORE_WARN(
						"Vulkan device loss was confirmed; every pending presentation request retired by its fence before child cleanup");
				else
					PF_CORE_WARN(
						"Vulkan device loss was confirmed; legacy presentation resources were released using the authorized best-effort "
						"shutdown path, which does not establish a formal presentation-engine completion guarantee");
			}
			else if (Result.Disposition == ShutdownDisposition::PresentationLifetimeUnknown)
			{
				PF_CORE_ERROR("Vulkan shutdown retained presentation resources because a presentation fence did not establish the required "
							  "resource-reference handoff (result {})",
					Result.VulkanResult);
			}
			else if (Result.Disposition == ShutdownDisposition::WaitFailed)
			{
				PF_CORE_ERROR("Vulkan shutdown wait failed at stage {} with outcome {} and VkResult {}",
					static_cast<uint32_t>(Result.Stage), static_cast<uint32_t>(Result.WaitOutcome), Result.VulkanResult);
			}
			else if (Result.Disposition == ShutdownDisposition::CleanupFailed)
			{
				PF_CORE_ERROR("Vulkan shutdown cleanup failed at stage {}", static_cast<uint32_t>(Result.Stage));
			}
			return Result;
		}

		bool IsShutdownComplete() const noexcept override { return m_ShutdownComplete; }

		bool HasDevice() const noexcept override { return static_cast<bool>(m_Device); }

		bool IsDeviceLost() const noexcept override { return m_DeviceHealth.IsLost(); }

		int32_t DeviceLossResult() const noexcept override { return m_DeviceHealth.VulkanResult(); }

		ShutdownWaitResult WaitForNvrhiIdle() noexcept override
		{
			if (!m_ActiveNvrhiDevice)
				return m_DeviceHealth.IsLost() ? ShutdownWaitResult{ ShutdownWaitOutcome::DeviceLost, m_DeviceHealth.VulkanResult() }
											   : ShutdownWaitResult{};
			try
			{
				if (m_ActiveNvrhiDevice->waitForIdle())
					return m_DeviceHealth.IsLost() ? ShutdownWaitResult{ ShutdownWaitOutcome::DeviceLost, m_DeviceHealth.VulkanResult() }
												   : ShutdownWaitResult{};
				// In this pinned NVRHI revision, false is returned only by its
				// vk::DeviceLostError handler in Device::waitForIdle().
				m_DeviceHealth.MarkLost();
			}
			catch (...)
			{
				return { m_DeviceHealth.IsLost() ? ShutdownWaitOutcome::DeviceLost : ShutdownWaitOutcome::Failed,
					m_DeviceHealth.VulkanResult() };
			}
			return { m_DeviceHealth.IsLost() ? ShutdownWaitOutcome::DeviceLost : ShutdownWaitOutcome::Failed,
				m_DeviceHealth.VulkanResult() };
		}

		ShutdownWaitResult WaitForDeviceIdle() noexcept override
		{
			if (!m_Device) return {};
			return ClassifyShutdownVkResult(VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(static_cast<VkDevice>(m_Device)));
		}

		ShutdownWaitResult WaitForPresentationRetirement() noexcept override
		{
			try
			{
				m_PresentationResources.WaitForRetirement(m_SwapchainImages, "main swapchain during shutdown");
				for (RetiredSwapchainGeneration& Generation : m_RetiredSwapchainGenerations)
					m_PresentationResources.WaitForRetirement(Generation.Images, "retired main swapchain during shutdown");
				for (auto& [TargetID, Host] : m_PlatformTargets)
				{
					(void)TargetID;
					if (!Host) continue;
					m_PresentationResources.WaitForRetirement(Host->Images, "native-window swapchain during shutdown");
					for (RetiredSwapchainGeneration& Generation : Host->RetiredGenerations)
						m_PresentationResources.WaitForRetirement(Generation.Images, "retired native-window swapchain during shutdown");
				}
			}
			catch (...)
			{
				return { m_DeviceHealth.IsLost() ? ShutdownWaitOutcome::DeviceLost : ShutdownWaitOutcome::Failed,
					m_DeviceHealth.VulkanResult() };
			}
			if (m_DeviceHealth.IsLost()) return { ShutdownWaitOutcome::DeviceLost, m_DeviceHealth.VulkanResult() };
			return {};
		}

		ShutdownWaitResult WaitForPresentationFencesAfterDeviceLoss() noexcept override
		{
			if (!UsesPresentationFences() || !m_DeviceHealth.IsLost()) return { ShutdownWaitOutcome::Failed, 0 };

			bool AllRetired = true;
			int32_t FirstFailure = 0;
			const auto WaitImages = [this, &AllRetired, &FirstFailure](std::vector<SwapchainImage>& Images)
			{
				for (SwapchainImage& Image : Images)
				{
					int32_t Result = VK_SUCCESS;
					if (!m_PresentationResources.WaitForPresentationAfterDeviceLoss(Image, Result))
					{
						AllRetired = false;
						if (FirstFailure == VK_SUCCESS) FirstFailure = Result;
					}
				}
			};
			WaitImages(m_SwapchainImages);
			for (RetiredSwapchainGeneration& Generation : m_RetiredSwapchainGenerations)
				WaitImages(Generation.Images);
			for (auto& [TargetID, Host] : m_PlatformTargets)
			{
				(void)TargetID;
				if (!Host) continue;
				WaitImages(Host->Images);
				for (RetiredSwapchainGeneration& Generation : Host->RetiredGenerations)
					WaitImages(Generation.Images);
			}
			if (AllRetired) return {};
			const ShutdownWaitOutcome Outcome =
				FirstFailure == VK_ERROR_DEVICE_LOST ? ShutdownWaitOutcome::DeviceLost : ShutdownWaitOutcome::Failed;
			return { Outcome, FirstFailure };
		}

		ShutdownWaitResult BestEffortWaitForLegacyPresentationAfterDeviceLoss() noexcept override
		{
			if (UsesPresentationFences() || !m_DeviceHealth.IsLost()) return { ShutdownWaitOutcome::Failed, 0 };
			PF_CORE_WARN("The Vulkan device is lost without maintenance presentation fences; performing the legacy queue-idle best-effort "
						 "shutdown. This does not prove presentation-engine resource release");
			const ShutdownWaitResult Result =
				ClassifyShutdownVkResult(VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueWaitIdle(static_cast<VkQueue>(m_PresentQueue)));
			if (Result.Outcome == ShutdownWaitOutcome::Failed)
				PF_CORE_WARN(
					"Legacy present-queue idle also failed with VkResult {}; continuing the authorized lost-device best-effort cleanup",
					Result.VulkanResult);
			return Result;
		}

		bool ReleasePartialInitialization() noexcept override
		{
			if (m_Device || m_Swapchain || !m_SwapchainImages.empty() || !m_RetiredSwapchainGenerations.empty() ||
				!m_PlatformTargets.empty())
				return false;
			m_PresentationResources.Reset();
			m_ActiveNvrhiDevice = nullptr;
			m_ValidationDevice = nullptr;
			m_VulkanDevice = nullptr;
			return ReleaseInstanceOwnedChildren();
		}

		bool ReleaseDeviceOwnedChildren(ShutdownPresentationReleaseMode Mode) noexcept override
		{
			if (!m_Device) return false;
			if ((Mode == ShutdownPresentationReleaseMode::HealthyDevice && m_DeviceHealth.IsLost()) ||
				(Mode == ShutdownPresentationReleaseMode::DeviceLostWithFencesRetired &&
					(!m_DeviceHealth.IsLost() || !UsesPresentationFences())) ||
				(Mode == ShutdownPresentationReleaseMode::LegacyBestEffortAfterDeviceLoss &&
					(!m_DeviceHealth.IsLost() || UsesPresentationFences())))
				return false;

			PresentationResourceReleaseMode ResourceMode = PresentationResourceReleaseMode::ShutdownAfterIdle;
			if (Mode == ShutdownPresentationReleaseMode::DeviceLostWithFencesRetired)
				ResourceMode = PresentationResourceReleaseMode::DeviceLostWithFencesRetired;
			else if (Mode == ShutdownPresentationReleaseMode::LegacyBestEffortAfterDeviceLoss)
				ResourceMode = PresentationResourceReleaseMode::LegacyBestEffortAfterDeviceLoss;

			try
			{
				m_ActiveRenderTarget.reset();
				ClearActiveRecordingState();
				for (auto& [TargetID, Host] : m_PlatformTargets)
				{
					(void)TargetID;
					if (!Host) continue;
					m_PresentationResources.ReleaseImages(Host->Images, ResourceMode);
					m_PresentationResources.ReleaseGenerations(Host->RetiredGenerations, ResourceMode);
					ReleasePlatformFrames(*Host);
					if (Host->Swapchain) m_Device.destroySwapchainKHR(Host->Swapchain);
					Host->Swapchain = vk::SwapchainKHR();
					if (m_Instance && Host->Surface) m_Instance.destroySurfaceKHR(Host->Surface);
					Host->Surface = vk::SurfaceKHR();
				}
				m_PlatformTargets.clear();
				m_PresentationResources.ReleaseImages(m_SwapchainImages, ResourceMode);
				m_PresentationResources.ReleaseGenerations(m_RetiredSwapchainGenerations, ResourceMode);
				for (FrameContext& Frame : m_Frames)
				{
					Frame.CommandList = nullptr;
					Frame.SubmissionValue = 0;
					if (Frame.ImageAvailable) m_Device.destroySemaphore(Frame.ImageAvailable);
					Frame.ImageAvailable = vk::Semaphore();
				}
				if (m_Swapchain) m_Device.destroySwapchainKHR(m_Swapchain);
				m_Swapchain = vk::SwapchainKHR();
				m_SwapchainExtent = vk::Extent2D{};
				m_PresentationResources.Reset();
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool ReleaseNvrhiDevice() noexcept override
		{
			m_ActiveNvrhiDevice = nullptr;
			m_ValidationDevice = nullptr;
			m_VulkanDevice = nullptr;
			return true;
		}

		void DestroyVulkanDevice() noexcept override
		{
			if (m_Device) m_Device.destroy();
			m_Device = vk::Device();
			m_GraphicsQueue = vk::Queue();
			m_PresentQueue = vk::Queue();
		}

		bool ReleaseInstanceOwnedChildren() noexcept override
		{
			if (m_Device || m_ValidationDevice || m_VulkanDevice || !m_PlatformTargets.empty()) return false;

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
			m_VulkanLoader.reset();
			m_ShutdownComplete = true;
			return true;
		}

	private:
		Window& m_Window;
		GLFWwindow* m_NativeWindow = nullptr;
		std::unique_ptr<vk::detail::DynamicLoader> m_VulkanLoader;
		VulkanDeviceHealth m_DeviceHealth;
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
		VulkanPresentationResources m_PresentationResources;
		std::vector<const char*> m_EnabledInstanceExtensions;
		std::vector<const char*> m_EnabledLayers;
		std::array<FrameContext, FramesInFlight> m_Frames;
		RecordingContext m_Recording;
		RecordingContext m_MainRecording;
		std::unordered_map<uint64_t, std::unique_ptr<PlatformPresentationHost>> m_PlatformTargets;
		PlatformPresentationHost* m_ActivePlatformTarget = nullptr;
		std::optional<ActiveRenderTarget> m_ActiveRenderTarget;
		std::vector<SwapchainImage> m_SwapchainImages;
		std::vector<RetiredSwapchainGeneration> m_RetiredSwapchainGenerations;
		vk::SwapchainKHR m_Swapchain;
		vk::Extent2D m_SwapchainExtent;
		vk::SurfaceFormatKHR m_SurfaceFormat;
		nvrhi::Format m_NvrhiFormat = nvrhi::Format::UNKNOWN;
		vk::PresentModeKHR m_PresentMode = vk::PresentModeKHR::eFifo;
		VulkanSupport::PresentationInstanceExtensionAvailability m_AvailablePresentationInstanceExtensions;
		VulkanSupport::PresentationInstanceExtensionPlan m_EnabledPresentationInstanceExtensions;
		std::optional<VulkanSupport::PresentationFenceCapabilityReason> m_StrictPresentationRejectionReason;
		VulkanSupport::PresentationPolicy m_PresentationPolicy = VulkanSupport::PresentationPolicy::Compatible;
		VulkanSupport::PresentationRetirementMechanism m_PresentationRetirementMechanism =
			VulkanSupport::PresentationRetirementMechanism::LegacyBestEffort;
		std::vector<const char*> m_EnabledDeviceExtensions;
		bool m_PortabilityEnumerationEnabled = false;
		bool m_PortabilitySubsetEnabled = false;
		bool m_FillModeNonSolidEnabled = false;
		uint32_t m_MinVertexInputBindingStrideAlignment = 1;
		bool m_SwapchainMaintenanceFeatureEnabled = false;
		bool m_ForceLegacyRetirementForTesting = false;
		bool m_FirstPresentationPending = false;
		bool m_FirstPresentationCompleted = false;
		bool m_RetirePreviousGenerationsAfterSubmit = false;
		bool m_DeferredRetirementLimitLogged = false;
		uint32_t m_CurrentFrame = 0;
		uint32_t m_AcquiredImageIndex = 0;
		uint32_t m_FirstPresentationImageIndex = 0;
		uint64_t m_ActivePlatformTargetID = 0;
		uint64_t m_NextPlatformTargetID = 0;
		bool m_DebugUtilsEnabled = false;
		bool m_KhronosValidationEnabled = false;
		bool m_SwapchainRecreationNeeded = false;
		bool m_SwapchainUnusable = false;
		bool m_LegacyRecoveryAttemptedAtLimit = false;
		bool m_SwapchainRetirementBlocked = false;
		bool m_SwapchainRetirementBlockedLogged = false;
		bool m_LoggedMainPresentationDuringNativeRetirementBlock = false;
		bool m_LoggedNativePresentationDuringMainRetirementBlock = false;
		size_t m_MainFramesDuringNativeRetirementBlock = 0;
		size_t m_NativeFramesDuringMainRetirementBlock = 0;
		bool m_AcquiredSuboptimal = false;
		bool m_LoggedFirstPresentedFrameForSwapchain = false;
		bool m_LoggedFirstDraw = false;
		bool m_FrameActive = false;
		bool m_ShutdownComplete = false;
#if defined(PF_DEBUG)
		bool m_InjectNextPlatformImageAllocationFailure = false;
		uint32_t m_DebugMainAcquireOutOfDateRemaining = 0;
		uint32_t m_DebugNativeAcquireOutOfDateCount = 0;
		uint32_t m_DebugMainAcquireOutOfDateAfterNativeTargetFrames = 0;
		uint32_t m_DebugMainAcquireOutOfDateAfterNativeTargetCount = 0;
		uint32_t m_DebugMainAcquireOutOfDateAfterNativeTargetDelayFrames = 60;
		uint32_t m_DebugMainSwapchainCreationFailureAtDeferredCount = 0;
		uint32_t m_DebugNativeSwapchainCreationFailureAtDeferredCount = 0;
		bool m_DebugMainSwapchainCreationFailureConsumed = false;
		bool m_DebugMainAcquireOutOfDateAfterNativeTarget = false;
#endif
	};

	std::unique_ptr<RendererBackend> CreateVulkanRenderer(Window& Window)
	{
		return std::make_unique<VulkanRenderer>(Window);
	}
} // namespace PulseForge
