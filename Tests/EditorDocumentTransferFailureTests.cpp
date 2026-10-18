#include "Editor/EditorDocumentManager.h"

#include <iostream>
#include <vector>

namespace
{
	bool AllocationFailureWasInjected = false;

	bool Require(bool Condition, const char* Message)
	{
		if (Condition) return true;
		std::cerr << Message << '\n';
		return false;
	}

	bool TestTransferPreparationAllocationFailure()
	{
		using namespace PulseForgeEditor;
		const PulseForge::UUID Project{ 0x5100000000000000ull, 1 };
		const PulseForge::UUID Asset{ 0x5200000000000000ull, 1 };
		const EditorDocumentIdentity Identity{ Project, Asset, EditorDocumentKind::Material };
		EditorDocumentManager Manager;
		const auto Opened = Manager.Open(Identity, "Assets/Material.material", "Material", EditorDocumentOpenPreference::MainDocumentStrip);
		const auto NativeHost = Manager.CreateNativeHost("native:allocation-test", EditorDocumentKind::Material);
		if (!Require(Opened.has_value() && NativeHost.has_value(), "could not prepare transfer allocation fixture")) return false;
		if (!Require(Manager.MarkEdited(Identity), "could not establish dirty document state")) return false;

		const auto MainBefore = Manager.FindHost(EditorDocumentManager::MainHostID)->Documents;
		const auto NativeBefore = Manager.FindHost(*NativeHost)->Documents;
		const std::string FocusBefore = Manager.GetFocusedHost()->ID;
		const bool PlacementsWereEmpty = Manager.GetRememberedPlacements(Project).empty();
		const uint64_t RevisionBefore = Manager.Find(Identity)->Revision;

		Manager.FailNextTransferStagingAllocationForTesting();
		const auto Transfer =
			Manager.Transfer(Identity, *NativeHost, 0, [](const EditorDocumentSession&, const EditorDocumentHost&) { return true; });

		AllocationFailureWasInjected = !Transfer && Transfer.error().find("atomic document transfer") != std::string::npos;
		if (!Require(AllocationFailureWasInjected, "the staged transfer allocation failure was not reported")) return false;
		const EditorDocumentSession* Session = Manager.Find(Identity);
		if (!Require(Session && Session->HostID == EditorDocumentManager::MainHostID && Session->Revision == RevisionBefore &&
						 Session->IsDirty(),
				"allocation failure changed session identity, host, revision, or dirty state"))
			return false;
		if (!Require(Manager.FindHost(EditorDocumentManager::MainHostID)->Documents == MainBefore &&
						 Manager.FindHost(*NativeHost)->Documents == NativeBefore && Manager.GetFocusedHost()->ID == FocusBefore,
				"allocation failure changed host membership or focus"))
			return false;
		if (!Require(PlacementsWereEmpty && Manager.GetRememberedPlacements(Project).empty(),
				"allocation failure changed remembered document placement"))
			return false;

		const auto Retry = Manager.Transfer(Identity, *NativeHost, 0);
		Session = Manager.Find(Identity);
		return Require(Retry && Session && Session->HostID == *NativeHost && Session->IsDirty() &&
						   Manager.FindHost(EditorDocumentManager::MainHostID)->Documents.empty() &&
						   Manager.FindHost(*NativeHost)->Documents == std::vector<EditorDocumentIdentity>{ Identity },
			"transfer retry did not succeed after the injected allocation failure");
	}
} // namespace

int main()
{
	return TestTransferPreparationAllocationFailure() ? 0 : 1;
}
