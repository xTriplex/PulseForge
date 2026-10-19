#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <nfd.h>
#include <nfd_glfw3.h>

#include "Editor/EditorPlatformServices.h"

#include <string>
#include <memory>
#include <exception>
#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		bool s_NativeFileDialogsInitialized = false;

		std::string PathToUtf8(const std::filesystem::path& Path)
		{
			const std::u8string Encoded = Path.u8string();
			return { reinterpret_cast<const char*>(Encoded.data()), Encoded.size() };
		}

		std::filesystem::path PathFromUtf8(const char* Path)
		{
			const auto* Bytes = reinterpret_cast<const char8_t*>(Path);
			return std::filesystem::path(std::u8string(Bytes, Bytes + std::char_traits<char>::length(Path)));
		}

		std::string GetNativeFileDialogError(std::string_view Context)
		{
			const char* Error = NFD_GetError();
			return std::string(Context) + ": " + (Error ? Error : "unknown NativeFileDialog error");
		}

		void FocusParent(GLFWwindow* Parent) noexcept
		{
			// Request focus restoration after the modal dialog; the window manager may ignore it.
			if (Parent) glfwFocusWindow(Parent);
		}

		class ParentFocusRestorer final
		{
		public:
			explicit ParentFocusRestorer(GLFWwindow* Parent) noexcept : m_Parent(Parent) {}

			~ParentFocusRestorer() noexcept { FocusParent(m_Parent); }

			ParentFocusRestorer(const ParentFocusRestorer&) = delete;
			ParentFocusRestorer& operator=(const ParentFocusRestorer&) = delete;

		private:
			GLFWwindow* m_Parent;
		};

		void MakeNativeFilter(
			const EditorFileDialogFilter* Filter, std::string& Name, std::string& Extensions, nfdu8filteritem_t& NativeFilter)
		{
			if (!Filter) return;
			Name.assign(Filter->Name);
			Extensions.assign(Filter->Extensions);
			NativeFilter = { Name.c_str(), Extensions.c_str() };
		}

		bool MakeNativeParent(GLFWwindow* Parent, nfdwindowhandle_t& NativeParent)
		{
			return !Parent || NFD_GetNativeWindowFromGLFWWindow(Parent, &NativeParent);
		}
	} // namespace

	std::expected<void, std::string> EditorPlatformServices::InitializeNativeFileDialogs()
	{
		if (s_NativeFileDialogsInitialized) return {};
		if (NFD_Init() != NFD_OKAY) return std::unexpected(GetNativeFileDialogError("Could not initialize native file dialogs"));

		if (!NFD_SetDisplayPropertiesFromGLFW())
		{
			const std::string Error = GetNativeFileDialogError("Could not configure native file dialogs for GLFW");
			NFD_Quit();
			return std::unexpected(Error);
		}

		s_NativeFileDialogsInitialized = true;
		return {};
	}

	void EditorPlatformServices::ShutdownNativeFileDialogs() noexcept
	{
		if (!s_NativeFileDialogsInitialized) return;
		NFD_Quit();
		s_NativeFileDialogsInitialized = false;
	}

	std::expected<std::optional<std::filesystem::path>, std::string> EditorPlatformServices::ShowOpenFileDialog(
		GLFWwindow* Parent, const EditorFileDialogFilter* Filter, const std::filesystem::path& DefaultPath)
	{
		if (!s_NativeFileDialogsInitialized) return std::unexpected("Native file dialogs have not been initialized");

		ParentFocusRestorer RestoreFocus(Parent);
		try
		{
			std::string FilterName;
			std::string FilterExtensions;
			nfdu8filteritem_t NativeFilter{};
			MakeNativeFilter(Filter, FilterName, FilterExtensions, NativeFilter);
			const std::string Utf8DefaultPath = DefaultPath.empty() ? std::string{} : PathToUtf8(DefaultPath);
			nfdwindowhandle_t NativeParent{};
			if (!MakeNativeParent(Parent, NativeParent))
				return std::unexpected("Could not attach the file dialog to its parent window on this platform");

			nfdu8char_t* SelectedPath = nullptr;
			nfdopendialogu8args_t Arguments{};
			Arguments.filterList = Filter ? &NativeFilter : nullptr;
			Arguments.filterCount = Filter ? 1 : 0;
			Arguments.defaultPath = Utf8DefaultPath.empty() ? nullptr : Utf8DefaultPath.c_str();
			Arguments.parentWindow = NativeParent;
			const nfdresult_t Result = NFD_OpenDialogU8_With(&SelectedPath, &Arguments);
			const std::unique_ptr<nfdu8char_t, decltype(&NFD_FreePathU8)> SelectedPathOwner(SelectedPath, &NFD_FreePathU8);
			if (Result == NFD_OKAY && SelectedPath) return std::optional<std::filesystem::path>(PathFromUtf8(SelectedPathOwner.get()));
			if (Result == NFD_ERROR) return std::unexpected(GetNativeFileDialogError("Could not open the file dialog"));
			if (Result == NFD_CANCEL) return std::optional<std::filesystem::path>{};
			return std::unexpected("The file dialog returned success without a selected path");
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not complete the open file dialog: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not complete the open file dialog because of an unknown exception");
		}
	}

	std::expected<std::optional<std::filesystem::path>, std::string> EditorPlatformServices::ShowSaveFileDialog(
		GLFWwindow* Parent, const EditorFileDialogFilter* Filter, const std::filesystem::path& DefaultPath, std::string_view DefaultName)
	{
		if (!s_NativeFileDialogsInitialized) return std::unexpected("Native file dialogs have not been initialized");

		ParentFocusRestorer RestoreFocus(Parent);
		try
		{
			std::string FilterName;
			std::string FilterExtensions;
			nfdu8filteritem_t NativeFilter{};
			MakeNativeFilter(Filter, FilterName, FilterExtensions, NativeFilter);
			const std::string Utf8DefaultPath = DefaultPath.empty() ? std::string{} : PathToUtf8(DefaultPath);
			const std::string DefaultNameString(DefaultName);
			nfdwindowhandle_t NativeParent{};
			if (!MakeNativeParent(Parent, NativeParent))
				return std::unexpected("Could not attach the save dialog to its parent window on this platform");

			nfdu8char_t* SelectedPath = nullptr;
			nfdsavedialogu8args_t Arguments{};
			Arguments.filterList = Filter ? &NativeFilter : nullptr;
			Arguments.filterCount = Filter ? 1 : 0;
			Arguments.defaultPath = Utf8DefaultPath.empty() ? nullptr : Utf8DefaultPath.c_str();
			Arguments.defaultName = DefaultNameString.empty() ? nullptr : DefaultNameString.c_str();
			Arguments.parentWindow = NativeParent;
			const nfdresult_t Result = NFD_SaveDialogU8_With(&SelectedPath, &Arguments);
			const std::unique_ptr<nfdu8char_t, decltype(&NFD_FreePathU8)> SelectedPathOwner(SelectedPath, &NFD_FreePathU8);
			if (Result == NFD_OKAY && SelectedPath) return std::optional<std::filesystem::path>(PathFromUtf8(SelectedPathOwner.get()));
			if (Result == NFD_ERROR) return std::unexpected(GetNativeFileDialogError("Could not open the save dialog"));
			if (Result == NFD_CANCEL) return std::optional<std::filesystem::path>{};
			return std::unexpected("The save dialog returned success without a selected path");
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not complete the save file dialog: ") + Exception.what());
		}
		catch (...)
		{
			return std::unexpected("Could not complete the save file dialog because of an unknown exception");
		}
	}

	bool EditorPlatformServices::CanPositionNativeWindows() noexcept
	{
#ifdef _WIN32
		return true;
#elif defined(__APPLE__)
		return true;
#else
		return glfwGetPlatform() == GLFW_PLATFORM_X11;
#endif
	}
} // namespace PulseForgeEditor
