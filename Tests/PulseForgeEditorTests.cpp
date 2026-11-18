#include "TestEditorSupport.h"

#include <spdlog/spdlog.h>
#include <iterator>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <cwchar>

namespace
{
	LONG WINAPI CaptureEditorTestCrash(EXCEPTION_POINTERS* ExceptionInfo) noexcept
	{
		wchar_t TempPath[MAX_PATH]{};
		const DWORD TempPathLength = GetTempPathW(MAX_PATH, TempPath);
		if (TempPathLength == 0 || TempPathLength >= MAX_PATH) return EXCEPTION_EXECUTE_HANDLER;

		wchar_t DumpPath[MAX_PATH]{};
		if (swprintf_s(DumpPath, std::size(DumpPath), L"%lsPulseForgeEditorTests-crash.dmp", TempPath) < 0)
			return EXCEPTION_EXECUTE_HANDLER;

		const HANDLE DumpFile = CreateFileW(DumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (DumpFile == INVALID_HANDLE_VALUE) return EXCEPTION_EXECUTE_HANDLER;

		MINIDUMP_EXCEPTION_INFORMATION ExceptionRecord{
			GetCurrentThreadId(), ExceptionInfo, FALSE
		};
		(void)MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), DumpFile, MiniDumpNormal,
			ExceptionInfo ? &ExceptionRecord : nullptr, nullptr, nullptr);
		CloseHandle(DumpFile);
		OutputDebugStringW(L"PulseForgeEditorTests wrote an unhandled-exception minidump to %TEMP%\\PulseForgeEditorTests-crash.dmp\n");
		return EXCEPTION_EXECUTE_HANDLER;
	}
}
#endif

void RunEditorDocumentHostTestEditorTests(TestRunner& Tests);
void RunPrefabLinkedInstanceEditorTests(TestRunner& Tests);
void RunEditorBuildProfileControllerTests(TestRunner& Tests);
void RunEditorAssetEditSchedulingTests(TestRunner& Tests);
void RunEditorInspectorRegressionTests(TestRunner& Tests);
void RunEditorScriptAuthoringInspectorTests(TestRunner& Tests);
void RunEditorRuntimeUIDocumentEditorTests(TestRunner& Tests);
void RunEditorGraphDocumentEditorTests(TestRunner& Tests);
#if defined(_DEBUG) || defined(PF_DEBUG)
void RunEditorDebugAcceptanceCoordinatorTests(TestRunner& Tests);
#endif

int main(int ArgumentCount, char** Arguments)
{
	#if defined(_WIN32)
	SetUnhandledExceptionFilter(CaptureEditorTestCrash);
	#endif
	PulseForge::Log::Init();
	TestRunner Tests;
	PF_CHECK(Tests, spdlog::get("PULSEFORGE") != nullptr);
	PF_CHECK(Tests, spdlog::get("APP") != nullptr);
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--runtime-ui-document")
	{
		RunEditorRuntimeUIDocumentEditorTests(Tests);
		return Tests.Finish();
	}
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--graph-document")
	{
		RunEditorGraphDocumentEditorTests(Tests);
		return Tests.Finish();
	}
	RunEditorHistoryTests(Tests);
	RunEditorViewportTests(Tests);
	RunEditorBuildProfileControllerTests(Tests);
	RunEditorViewportSceneRenderViewTests(Tests);
	RunEditorAssetPreviewStressHarnessTests(Tests);
	RunEditorAssetEditSchedulingTests(Tests);
	RunEditorDocumentHostTestEditorTests(Tests);
	RunPrefabLinkedInstanceEditorTests(Tests);
#if defined(_DEBUG) || defined(PF_DEBUG)
	RunEditorDebugAcceptanceCoordinatorTests(Tests);
#endif
	RunEditorTests(Tests);
	RunEditorStateTests(Tests);
	RunEditorDocumentManagerTests(Tests);
	RunEditorDocumentEditorRegistryTests(Tests);
	RunEditorSnapshotHistoryTests(Tests);
	RunEditorDocumentWorkspaceTests(Tests);
	RunEditorAssetImportOperationTests(Tests);
	RunEditorInspectorRegressionTests(Tests);
	RunEditorScriptAuthoringInspectorTests(Tests);
	RunEditorRuntimeUIDocumentEditorTests(Tests);
	RunEditorGraphDocumentEditorTests(Tests);
	RunProjectRuntimeTests(Tests);
	return Tests.Finish();
}
