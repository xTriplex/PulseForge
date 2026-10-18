#include "Editor/EditorCommandHistory.h"

#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <string_view>

namespace
{
	bool FailNextAllocation = false;
	bool AllocationWasFailed = false;

	bool Require(bool Condition, std::string_view Message)
	{
		if (Condition) return true;
		std::cerr << Message << '\n';
		return false;
	}

	bool TestCandidateCopyFailurePreservesRedo()
	{
		using PulseForgeEditor::EditorCommandHistory;
		EditorCommandHistory History;
		History.Reset();
		if (!History.Begin("first", "a") || !History.Commit("b")) return Require(false, "could not create the initial command history");
		if (!History.Begin("second", "b") || !History.Commit("c")) return Require(false, "could not create the redo command");
		History.MarkSaved();
		std::string Current = "c";
		const auto Restore = [&Current](std::string_view State) -> std::expected<void, std::string>
		{
			Current.assign(State);
			return {};
		};
		if (!History.Undo(Restore) || Current != "b" || !History.CanRedo())
			return Require(false, "could not establish a saved history state with redo available");
		const size_t StoredBytes = History.GetStoredBytes();
		const size_t CommandCount = History.GetCommandCount();
		const size_t Cursor = History.GetCursor();
		const bool WasDirty = History.IsDirty();
		const std::string Before(128, 'b');
		std::string After(128, 'c');
		if (!History.Begin("d", Before)) return Require(false, "could not begin the allocation-failure transaction");

		FailNextAllocation = true;
		AllocationWasFailed = false;
		const auto FailedCommit = History.Commit(std::move(After));
		FailNextAllocation = false;
		if (!Require(AllocationWasFailed && !FailedCommit && History.IsActive(),
				"candidate-copy failure did not preserve the active transaction"))
			return false;
		if (!Require(History.GetCommandCount() == CommandCount && History.GetCursor() == Cursor &&
						 History.GetStoredBytes() == StoredBytes && History.IsDirty() == WasDirty,
				"candidate-copy failure changed retained history, accounting, or dirty state"))
			return false;

		std::string Restored;
		if (!History.Cancel(
				[&Restored](std::string_view State) -> std::expected<void, std::string>
				{
					Restored.assign(State);
					return {};
				}) ||
			Restored != Before || History.IsActive())
			return Require(false, "candidate-copy failure lost the rollback snapshot");
		if (!Require(History.CanRedo(), "candidate-copy failure discarded the redo command")) return false;
		if (!History.Redo(Restore) || Current != "c" || History.IsDirty())
			return Require(false, "failed commit did not preserve the saved redo identity");
		return true;
	}

	bool TestVectorGrowthFailurePreservesActiveSnapshot()
	{
		using PulseForgeEditor::EditorCommandHistory;
		EditorCommandHistory History;
		History.Reset();
		History.MarkSaved();
		for (size_t Attempt = 0; Attempt < History.GetMaximumCommands(); ++Attempt)
		{
			const std::string Before(1, (Attempt % 2) == 0 ? 'a' : 'b');
			const std::string After(1, (Attempt % 2) == 0 ? 'b' : 'a');
			if (!History.Begin("x", Before)) return Require(false, "could not begin vector-growth fixture");
			FailNextAllocation = true;
			AllocationWasFailed = false;
			const auto Result = History.Commit(After);
			const bool FailedByAllocator = AllocationWasFailed;
			FailNextAllocation = false;
			if (!FailedByAllocator)
			{
				if (!Result) return Require(false, "commit rejected before reaching vector growth");
				continue;
			}

			if (!Require(!Result && History.IsActive(), "vector-growth allocation failure did not leave the edit recoverable"))
				return false;
			std::string Restored;
			const auto Cancelled = History.Cancel(
				[&Restored](std::string_view State) -> std::expected<void, std::string>
				{
					Restored.assign(State);
					return {};
				});
			if (!Require(Cancelled && Restored == Before && !History.IsActive(), "vector-growth failure lost the active rollback snapshot"))
				return false;
			if (!Require(History.GetCommandCount() == History.GetCursor() && History.GetStoredBytes() <= History.GetMaximumBytes(),
					"vector-growth failure corrupted the command cursor or byte accounting"))
				return false;
			return true;
		}
		return Require(false, "the allocation-failure fixture did not reach vector growth");
	}
} // namespace

void* operator new(std::size_t Size)
{
	if (FailNextAllocation)
	{
		FailNextAllocation = false;
		AllocationWasFailed = true;
		throw std::bad_alloc();
	}
	if (void* Memory = std::malloc(Size == 0 ? 1 : Size)) return Memory;
	throw std::bad_alloc();
}

void* operator new[](std::size_t Size)
{
	if (FailNextAllocation)
	{
		FailNextAllocation = false;
		AllocationWasFailed = true;
		throw std::bad_alloc();
	}
	if (void* Memory = std::malloc(Size == 0 ? 1 : Size)) return Memory;
	throw std::bad_alloc();
}

void operator delete(void* Memory) noexcept
{
	std::free(Memory);
}
void operator delete[](void* Memory) noexcept
{
	std::free(Memory);
}
void operator delete(void* Memory, std::size_t) noexcept
{
	std::free(Memory);
}
void operator delete[](void* Memory, std::size_t) noexcept
{
	std::free(Memory);
}

int main()
{
	if (!TestCandidateCopyFailurePreservesRedo()) return 1;
	if (!TestVectorGrowthFailurePreservesActiveSnapshot()) return 1;
	return 0;
}
