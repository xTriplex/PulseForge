#pragma once

#include <iostream>
#include <string_view>

class TestRunner
{
public:
	void Check(bool Passed, std::string_view Expression, const char* File, int Line)
	{
		++m_AssertionCount;
		if (Passed) return;

		++m_FailureCount;
		std::cerr << File << ':' << Line << ": check failed: " << Expression << '\n';
	}

	int Finish() const
	{
		if (m_FailureCount == 0) std::cout << "Passed " << m_AssertionCount << " checks.\n";
		else std::cerr << m_FailureCount << " of " << m_AssertionCount << " checks failed.\n";

		return m_FailureCount == 0 ? 0 : 1;
	}

private:
	int m_AssertionCount = 0;
	int m_FailureCount = 0;
};

#define PF_CHECK(Runner, Expression) (Runner).Check(static_cast<bool>(Expression), #Expression, __FILE__, __LINE__)

void RunCoreTests(TestRunner& Tests);
void RunSceneTests(TestRunner& Tests);
void RunSceneAssetTests(TestRunner& Tests);
void RunEditorTests(TestRunner& Tests);
void RunEditorViewportTests(TestRunner& Tests);
void RunEditorAssetPreviewStressHarnessTests(TestRunner& Tests);
void RunEditorStateTests(TestRunner& Tests);
void RunAssetsTests(TestRunner& Tests);
void RunAssetRegistryTests(TestRunner& Tests);
void RunAssetOperationsTests(TestRunner& Tests);
void RunDocumentsTests(TestRunner& Tests);
void RunProjectRuntimeTests(TestRunner& Tests);
void RunRendererTests(TestRunner& Tests);
void RunRenderMathTests(TestRunner& Tests);
void RunPointShadowImageAnalysisTests(TestRunner& Tests);
void RunEditorHistoryTests(TestRunner& Tests);
void RunAssetImportTests(TestRunner& Tests);
void RunAudioRuntimeTests(TestRunner& Tests);
