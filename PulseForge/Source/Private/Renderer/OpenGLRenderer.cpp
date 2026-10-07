#include "Core/PulseForgePCH.h"
#include "Renderer/RendererBackend.h"
#include "Window/Window.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <limits>
#include <stdexcept>

namespace PulseForge
{
	namespace
	{
		class OpenGLBuffer final : public Buffer
		{
		public:
			OpenGLBuffer(BufferDesc Description, GLuint Handle)
				: m_Description(std::move(Description)),
				  m_Handle(Handle)
			{
			}

			~OpenGLBuffer() override
			{
				if (m_Handle != 0)
					glDeleteBuffers(1, &m_Handle);
			}

			const BufferDesc& GetDescription() const noexcept override
			{
				return m_Description;
			}

		private:
			BufferDesc m_Description;
			GLuint m_Handle = 0;
		};
	}

	class OpenGLRenderer final : public RendererBackend
	{
	public:
		explicit OpenGLRenderer(Window& Window)
			: m_NativeWindow(static_cast<GLFWwindow*>(Window.GetNativeWindow()))
		{
			if (!m_NativeWindow)
				throw std::runtime_error("OpenGL renderer requires a valid GLFW window");

			glfwMakeContextCurrent(m_NativeWindow);
			if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
			{
				PF_CORE_ERROR("Failed to initialize OpenGL entry points");
				glfwMakeContextCurrent(nullptr);
				throw std::runtime_error("OpenGL function loading failed");
			}

			glfwSwapInterval(1);
		}

		bool BeginFrame() override
		{
			glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			return true;
		}

		void EndFrame() override
		{
			glfwSwapBuffers(m_NativeWindow);
		}

		RenderTargetCreateResult CreateRenderTarget(const RenderTargetDesc&) override
		{
			return std::unexpected(RenderTargetError{
				RenderTargetErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge render targets"
			});
		}

		GraphicsResult BeginRenderTarget(const RenderTarget&, const RenderTargetClearValue&) override
		{
			return std::unexpected(GraphicsError{
				GraphicsErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge render targets"
			});
		}

		GraphicsResult EndRenderTarget() override
		{
			return std::unexpected(GraphicsError{
				GraphicsErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge render targets"
			});
		}

		BufferCreateResult CreateBuffer(
			const BufferDesc& Description,
			std::span<const std::byte> InitialData) override
		{
			if (Description.Usage == BufferUsage::Constant)
			{
				return std::unexpected(BufferCreateError{
					BufferCreateErrorCode::UnsupportedFeature,
					"The transitional OpenGL backend does not support PulseForge constant buffers"
				});
			}

			if (Description.ByteSize > static_cast<uint64_t>(std::numeric_limits<GLsizeiptr>::max()))
			{
				return std::unexpected(BufferCreateError{
					BufferCreateErrorCode::BackendFailure,
					"OpenGL buffer size exceeds the GLsizeiptr limit"
				});
			}

			const GLenum Target = Description.Usage == BufferUsage::Vertex
				? GL_ARRAY_BUFFER
				: GL_ELEMENT_ARRAY_BUFFER;
			GLuint Handle = 0;
			glGenBuffers(1, &Handle);
			if (Handle == 0)
			{
				return std::unexpected(BufferCreateError{
					BufferCreateErrorCode::BackendFailure,
					"OpenGL failed to allocate a buffer object"
				});
			}

			glBindBuffer(Target, Handle);
			glBufferData(
				Target,
				static_cast<GLsizeiptr>(Description.ByteSize),
				InitialData.empty() ? nullptr : InitialData.data(),
				GL_STATIC_DRAW);
			glBindBuffer(Target, 0);

			const GLenum Error = glGetError();
			if (Error != GL_NO_ERROR)
			{
				glDeleteBuffers(1, &Handle);
				const std::string Message = "OpenGL failed to create the buffer; glGetError returned " +
					std::to_string(Error);
				PF_CORE_ERROR("{0}", Message);
				return std::unexpected(BufferCreateError{
					BufferCreateErrorCode::BackendFailure,
					Message
				});
			}

			return std::make_unique<OpenGLBuffer>(Description, Handle);
		}

		BufferUpdateResult WriteBuffer(const Buffer&, uint64_t, std::span<const std::byte>) override
		{
			return std::unexpected(BufferUpdateError{
				BufferUpdateErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge constant-buffer updates"
			});
		}

		BindingLayoutCreateResult CreateBindingLayout(const BindingLayoutDesc&) override
		{
			return std::unexpected(BindingError{
				BindingErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge binding layouts"
			});
		}

		TextureCreateResult CreateTexture(const TextureDesc&, std::span<const std::byte>) override
		{
			return std::unexpected(TextureError{
				TextureErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge shader textures yet"
			});
		}

		SamplerCreateResult CreateSampler(const SamplerDesc&) override
		{
			return std::unexpected(TextureError{
				TextureErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge shader samplers yet"
			});
		}

		BindingSetCreateResult CreateBindingSet(const BindingSetDesc&) override
		{
			return std::unexpected(BindingError{
				BindingErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge binding sets"
			});
		}

		ShaderCreateResult CreateShader(const ShaderDesc&, std::span<const std::byte>) override
		{
			return std::unexpected(GraphicsError{
				GraphicsErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support the PulseForge SPIR-V shader path"
			});
		}

		GraphicsPipelineCreateResult CreateGraphicsPipeline(const GraphicsPipelineDesc&) override
		{
			return std::unexpected(GraphicsError{
				GraphicsErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge graphics pipelines yet"
			});
		}

		GraphicsResult Draw(
			const GraphicsPipeline&,
			const Buffer&,
			const DrawArguments&,
			std::span<const BindingSet* const>) override
		{
			return std::unexpected(GraphicsError{
				GraphicsErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge draw commands yet"
			});
		}

		GraphicsResult DrawIndexed(
			const GraphicsPipeline&,
			const Buffer&,
			const Buffer&,
			const DrawIndexedArguments&,
			std::span<const BindingSet* const>) override
		{
			return std::unexpected(GraphicsError{
				GraphicsErrorCode::UnsupportedFeature,
				"The transitional OpenGL backend does not support PulseForge indexed draw commands yet"
			});
		}

	private:
		GLFWwindow* m_NativeWindow = nullptr;
	};

	std::unique_ptr<RendererBackend> CreateOpenGLRenderer(Window& Window)
	{
		return std::make_unique<OpenGLRenderer>(Window);
	}
}
