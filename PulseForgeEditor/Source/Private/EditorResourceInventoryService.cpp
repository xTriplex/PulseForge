#include "Editor/EditorResourceInventoryService.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetDependencyExtractor.h"

#include <limits>
#include <exception>
#include <utility>

namespace PulseForgeEditor
{
	namespace
	{
		[[nodiscard]] EditorResourceInventoryResult BuildProjectInventory(
			const std::filesystem::path& ProjectRoot,
			const PulseForge::UUID& ProjectID,
			uint64_t Generation,
			const EditorResourceInventoryLimits& Limits,
			std::stop_token CancellationToken)
		{
			EditorResourceInventoryResult Result;
			Result.ProjectIdentity = GetEditorResourceInventoryProjectIdentity(ProjectRoot, ProjectID);
			Result.Generation = Generation;
			if (Generation == 0 || ProjectID.IsNil())
				Result.Error = "Resource inventory request requires a project identity and nonzero generation";
			else
			{
				PulseForge::AssetDependencyCatalogBuildOptions BuildOptions;
				BuildOptions.CancellationToken = CancellationToken;
				auto Catalog = PulseForge::AssetDependencyCatalogBuilder::Build(
					ProjectRoot, ProjectID, Generation, BuildOptions);
				if (!Catalog)
				{
					Result.Cancelled = Catalog.error().Cancelled || CancellationToken.stop_requested();
					if (!Result.Cancelled) Result.Error = Catalog.error().Message;
				}
				else
				{
					Result.Snapshot = BuildEditorResourceInventory(
						**Catalog, ProjectRoot / ".pulseforge" / "DerivedData", Limits, CancellationToken);
					Result.Cancelled = Result.Snapshot->Cancelled;
					if (Result.Cancelled) Result.Snapshot.reset();
				}
			}
			return Result;
		}
	}

	EditorResourceInventoryService::EditorResourceInventoryService(Builder Build)
		: m_Builder(Build ? std::move(Build) : Builder(BuildProjectInventory)),
		  m_Worker([this](std::stop_token StopToken) { WorkerMain(StopToken); })
	{
	}

	EditorResourceInventoryService::~EditorResourceInventoryService()
	{
		{
			std::scoped_lock Lock(m_Mutex);
			m_Stopping = true;
			if (m_Pending) m_Pending->Cancellation.request_stop();
			if (m_ActiveCancellation) m_ActiveCancellation->request_stop();
		}
		m_Worker.request_stop();
		m_WorkAvailable.notify_all();
		if (m_Worker.joinable()) m_Worker.join();
	}

	void EditorResourceInventoryService::RequestRefresh(
		std::filesystem::path ProjectRoot,
		PulseForge::UUID ProjectID,
		uint64_t Generation,
		EditorResourceInventoryLimits Limits)
	{
		std::scoped_lock Lock(m_Mutex);
		if (m_Stopping) return;
		if (m_LatestSerial == std::numeric_limits<uint64_t>::max()) m_LatestSerial = 0;
		const uint64_t Serial = ++m_LatestSerial;
		if (m_Pending) m_Pending->Cancellation.request_stop();
		if (m_ActiveCancellation) m_ActiveCancellation->request_stop();
		m_Completed.reset();
		Request Work;
		Work.ProjectRoot = std::move(ProjectRoot);
		Work.ProjectID = ProjectID;
		Work.Generation = Generation;
		Work.Serial = Serial;
		Work.Limits = Limits;
		m_Pending = std::move(Work);
		m_WorkAvailable.notify_one();
	}

	void EditorResourceInventoryService::Cancel()
	{
		std::scoped_lock Lock(m_Mutex);
		if (m_LatestSerial == std::numeric_limits<uint64_t>::max()) m_LatestSerial = 0;
		else ++m_LatestSerial;
		if (m_Pending) m_Pending->Cancellation.request_stop();
		if (m_ActiveCancellation) m_ActiveCancellation->request_stop();
		m_Pending.reset();
		m_Completed.reset();
	}

	std::optional<EditorResourceInventoryResult> EditorResourceInventoryService::Poll()
	{
		std::scoped_lock Lock(m_Mutex);
		if (!m_Completed) return std::nullopt;
		auto Result = std::move(m_Completed);
		m_Completed.reset();
		return Result;
	}

	void EditorResourceInventoryService::WorkerMain(std::stop_token StopToken) noexcept
	{
		while (!StopToken.stop_requested())
		{
			Request Work;
			{
				std::unique_lock Lock(m_Mutex);
				if (!m_WorkAvailable.wait(Lock, StopToken, [&] { return m_Stopping || m_Pending.has_value(); }))
					return;
				if (m_Stopping || StopToken.stop_requested()) return;
				Work = std::move(*m_Pending);
				m_Pending.reset();
				m_ActiveCancellation = Work.Cancellation;
				m_ActiveSerial = Work.Serial;
			}

			EditorResourceInventoryResult Result;
			try
			{
				Result = m_Builder(Work.ProjectRoot, Work.ProjectID, Work.Generation, Work.Limits,
					Work.Cancellation.get_token());
			}
			catch (const std::exception& Exception)
			{
				Result.Error = Exception.what();
			}
			catch (...)
			{
				Result.Error = "Resource inventory worker failed with an unknown exception";
			}

			const std::string RequestedProjectIdentity = GetEditorResourceInventoryProjectIdentity(
				Work.ProjectRoot, Work.ProjectID);
			const bool ResultIdentityMismatch = !Result.ProjectIdentity.empty() &&
				Result.ProjectIdentity != RequestedProjectIdentity;
			const bool ResultGenerationMismatch = Result.Generation != 0 && Result.Generation != Work.Generation;
			const bool SnapshotIdentityMismatch = Result.Snapshot &&
				Result.Snapshot->ProjectIdentity != RequestedProjectIdentity;
			const bool SnapshotGenerationMismatch = Result.Snapshot && Result.Snapshot->Generation != Work.Generation;
			if (ResultIdentityMismatch || ResultGenerationMismatch || SnapshotIdentityMismatch || SnapshotGenerationMismatch)
			{
				Result.Snapshot.reset();
				if (Result.Error.empty())
					Result.Error = "Resource inventory builder returned a result for a different project or generation";
			}
			Result.ProjectIdentity = RequestedProjectIdentity;
			Result.Generation = Work.Generation;

			{
				std::scoped_lock Lock(m_Mutex);
				if (m_ActiveSerial == Work.Serial)
				{
					m_ActiveCancellation.reset();
					m_ActiveSerial = 0;
				}
				if (!m_Stopping && Work.Serial == m_LatestSerial && !Work.Cancellation.stop_requested())
					m_Completed = std::move(Result);
			}
		}
	}
} // namespace PulseForgeEditor
