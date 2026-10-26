#include "TestSupport.h"

#include "Renderer/AssetCache/AssetUploadBudget.h"

void RunAssetUploadBudgetTests(TestRunner& Tests)
{
	using namespace PulseForge;
	AssetUploadBudget Budget;
	PF_CHECK(Tests, Budget.TryReserve(32ull * 1024ull * 1024ull));
	PF_CHECK(Tests, Budget.GetUsedBytes() == 32ull * 1024ull * 1024ull);
	PF_CHECK(Tests, Budget.GetUsedOperations() == 1);
	PF_CHECK(Tests, !Budget.TryReserve(40ull * 1024ull * 1024ull));
	PF_CHECK(Tests, Budget.GetUsedOperations() == 1);
	for (size_t Operation = 0; Operation < AssetUploadBudget::DefaultMaximumOperations - 1; ++Operation)
		PF_CHECK(Tests, Budget.TryReserve(1));
	PF_CHECK(Tests, !Budget.TryReserve(1));
	Budget.BeginFrame();
	PF_CHECK(Tests, Budget.TryReserve(AssetUploadBudget::MaximumIndivisibleProductBytes));
	PF_CHECK(Tests, !Budget.TryReserve(1));
	Budget.BeginFrame();
	PF_CHECK(Tests, !Budget.TryReserve(AssetUploadBudget::MaximumIndivisibleProductBytes + 1));
	PF_CHECK(Tests, Budget.GetUsedBytes() == 0 && Budget.GetUsedOperations() == 0);
}
