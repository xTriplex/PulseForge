#include "TestAssetSupport.h"

#include "Assets/AssetDependencyCatalog.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
	using namespace PulseForge;

	class AssetDependencyPublicationFixture final
	{
	public:
		AssetDependencyPublicationFixture()
		{
			const auto Identifier = UUID::Generate();
			if (!Identifier) return;
			ProjectID = *Identifier;
			std::error_code Error;
			Root = std::filesystem::temp_directory_path(Error) / ("PulseForgeDependencyPublication-" + ProjectID.ToString());
			if (Error) return;
			std::filesystem::create_directories(Root / "Assets", Error);
			Valid = !Error;
		}

		~AssetDependencyPublicationFixture()
		{
			std::error_code Error;
			if (!Root.empty()) std::filesystem::remove_all(Root, Error);
		}

		[[nodiscard]] std::optional<AssetID> WriteManaged(std::string_view RelativePath, std::string_view Data)
		{
			const std::filesystem::path Path = Root / std::filesystem::u8path(RelativePath);
			std::error_code Error;
			std::filesystem::create_directories(Path.parent_path(), Error);
			if (Error || !WriteFile(Path, Data)) return std::nullopt;
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			if (!Metadata) return std::nullopt;
			return Metadata->ID;
		}

		[[nodiscard]] std::filesystem::path Absolute(std::string_view RelativePath) const
		{
			return Root / std::filesystem::u8path(RelativePath);
		}

		[[nodiscard]] static bool WriteFile(const std::filesystem::path& Path, std::string_view Data)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
			Output.close();
			return static_cast<bool>(Output);
		}

		[[nodiscard]] static std::optional<std::string> ReadFile(const std::filesystem::path& Path)
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) return std::nullopt;
			return std::string(std::istreambuf_iterator<char>(Input), {});
		}

		UUID ProjectID;
		std::filesystem::path Root;
		bool Valid = false;
	};

	[[nodiscard]] std::string Lowercase(std::string Value)
	{
		std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Character)
			{ return static_cast<char>(std::tolower(Character)); });
		return Value;
	}

	[[nodiscard]] bool Mentions(const AssetDependencyCatalogError& Error, std::string_view Needle)
	{
		return Lowercase(Error.Path.generic_string() + " " + Error.Message).find(Lowercase(std::string(Needle))) != std::string::npos;
	}

	[[nodiscard]] bool IsActionable(const AssetDependencyCatalogError& Error)
	{
		return !Error.Path.empty() && !Error.Message.empty();
	}

	[[nodiscard]] bool CreatePublishedBaseline(AssetDependencyPublicationFixture& Fixture, AssetDependencyCatalog& Catalog,
		const AssetID& Asset, TestRunner& Tests)
	{
		AssetDependencyCatalogTicket Ticket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto Candidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, Ticket.Generation);
		PF_CHECK(Tests, Candidate.has_value());
		if (!Candidate) return false;
		const auto Published = Catalog.Publish(Ticket, *Candidate);
		PF_CHECK(Tests, Published.has_value());
		PF_CHECK(Tests, Catalog.GetSnapshot() == *Candidate);
		PF_CHECK(Tests, (*Candidate)->GetRegistry().Find(Asset).has_value());
		return Published.has_value();
	}

	void TestPublishRejectsDuplicateUUIDAddedAfterBuild(TestRunner& Tests)
	{
		AssetDependencyPublicationFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		const auto ExistingID = Fixture.WriteManaged("Assets/Textures/Existing.ppm", "P3\n1 1\n255\n10 20 30\n");
		PF_CHECK(Tests, ExistingID.has_value());
		if (!ExistingID) return;

		AssetDependencyCatalog Catalog;
		const AssetDependencyCatalogTicket InitialTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto InitialCandidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, InitialTicket.Generation);
		PF_CHECK(Tests, InitialCandidate.has_value());
		if (!InitialCandidate) return;
		PF_CHECK(Tests, Catalog.Publish(InitialTicket, *InitialCandidate));
		const auto RetainedSnapshot = Catalog.GetSnapshot();
		PF_CHECK(Tests, RetainedSnapshot == *InitialCandidate);
		if (!RetainedSnapshot) return;

		const AssetDependencyCatalogTicket CandidateTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto Candidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, CandidateTicket.Generation);
		PF_CHECK(Tests, Candidate.has_value());
		if (!Candidate) return;
		PF_CHECK(Tests, (*Candidate)->GetRegistry().GetAssetCount() == 1);

		const auto DuplicateID = Fixture.WriteManaged("Assets/Textures/LateDuplicate.ppm", "P3\n1 1\n255\n40 50 60\n");
		PF_CHECK(Tests, DuplicateID.has_value());
		if (!DuplicateID) return;
		const std::filesystem::path DuplicateSidecar =
			AssetMetadataSerializer::GetSidecarPath(Fixture.Absolute("Assets/Textures/LateDuplicate.ppm"));
		std::ifstream MetadataInput(DuplicateSidecar, std::ios::binary);
		const std::string OriginalMetadata(std::istreambuf_iterator<char>(MetadataInput), {});
		std::string DuplicateMetadata = OriginalMetadata;
		const size_t GeneratedIDPosition = DuplicateMetadata.find(DuplicateID->ToString());
		PF_CHECK(Tests, GeneratedIDPosition != std::string::npos);
		if (GeneratedIDPosition == std::string::npos) return;
		DuplicateMetadata.replace(GeneratedIDPosition, DuplicateID->ToString().size(), ExistingID->ToString());
		PF_CHECK(Tests, AssetDependencyPublicationFixture::WriteFile(DuplicateSidecar, DuplicateMetadata));

		const auto Rejected = Catalog.Publish(CandidateTicket, *Candidate);
		PF_CHECK(Tests, !Rejected.has_value());
		if (!Rejected)
		{
			PF_CHECK(Tests, IsActionable(Rejected.error()));
			PF_CHECK(Tests, Mentions(Rejected.error(), "LateDuplicate.ppm"));
			PF_CHECK(Tests, Mentions(Rejected.error(), "UUID"));
			PF_CHECK(Tests, Mentions(Rejected.error(), ExistingID->ToString()));
		}
		PF_CHECK(Tests, Catalog.GetSnapshot() == RetainedSnapshot);
		PF_CHECK(Tests, RetainedSnapshot->GetRegistry().Find(*ExistingID).has_value());
		PF_CHECK(Tests, !RetainedSnapshot->GetRegistry().Find(*DuplicateID).has_value());
	}

	void TestPublishRejectsMalformedNewMetadata(TestRunner& Tests)
	{
		AssetDependencyPublicationFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const auto ExistingID = Fixture.WriteManaged("Assets/Textures/Existing.ppm", "P3\n1 1\n255\n10 20 30\n");
		PF_CHECK(Tests, ExistingID.has_value());
		if (!ExistingID) return;

		AssetDependencyCatalog Catalog;
		if (!CreatePublishedBaseline(Fixture, Catalog, *ExistingID, Tests)) return;
		const auto Retained = Catalog.GetSnapshot();
		const AssetDependencyCatalogTicket Ticket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto Candidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, Ticket.Generation);
		PF_CHECK(Tests, Candidate.has_value());
		if (!Candidate) return;

		const std::filesystem::path NewSource = Fixture.Absolute("Assets/Textures/LateMalformed.ppm");
		PF_CHECK(Tests, AssetDependencyPublicationFixture::WriteFile(NewSource, "P3\n1 1\n255\n40 50 60\n"));
		const std::filesystem::path NewSidecar = AssetMetadataSerializer::GetSidecarPath(NewSource);
		PF_CHECK(Tests, AssetDependencyPublicationFixture::WriteFile(NewSidecar, "{ malformed metadata"));

		const auto Rejected = Catalog.Publish(Ticket, *Candidate);
		PF_CHECK(Tests, !Rejected.has_value());
		if (!Rejected)
		{
			PF_CHECK(Tests, IsActionable(Rejected.error()));
			PF_CHECK(Tests, Mentions(Rejected.error(), "LateMalformed.ppm"));
			PF_CHECK(Tests, Mentions(Rejected.error(), "metadata"));
		}
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);
		PF_CHECK(Tests, Retained->GetRegistry().Find(*ExistingID).has_value());
	}

	void TestPublishRejectsMembershipChanges(TestRunner& Tests)
	{
		AssetDependencyPublicationFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const auto ExistingID = Fixture.WriteManaged("Assets/Textures/Existing.ppm", "P3\n1 1\n255\n10 20 30\n");
		PF_CHECK(Tests, ExistingID.has_value());
		if (!ExistingID) return;

		AssetDependencyCatalog Catalog;
		if (!CreatePublishedBaseline(Fixture, Catalog, *ExistingID, Tests)) return;
		const auto Retained = Catalog.GetSnapshot();
		if (!Retained) return;

		const AssetDependencyCatalogTicket AdditionTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto AdditionCandidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, AdditionTicket.Generation);
		PF_CHECK(Tests, AdditionCandidate.has_value());
		if (!AdditionCandidate) return;
		const auto AddedID = Fixture.WriteManaged("Assets/Textures/LateAddition.ppm", "P3\n1 1\n255\n70 80 90\n");
		PF_CHECK(Tests, AddedID.has_value());
		if (!AddedID) return;
		const auto AdditionRejected = Catalog.Publish(AdditionTicket, *AdditionCandidate);
		PF_CHECK(Tests, !AdditionRejected.has_value());
		if (!AdditionRejected)
		{
			PF_CHECK(Tests, IsActionable(AdditionRejected.error()));
			PF_CHECK(Tests, Mentions(AdditionRejected.error(), "registry") || Mentions(AdditionRejected.error(), "membership") ||
				Mentions(AdditionRejected.error(), "LateAddition.ppm"));
		}
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);

		const AssetDependencyCatalogTicket RefreshedAdditionTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto RefreshedAddition = AssetDependencyCatalogBuilder::Build(
			Fixture.Root, Fixture.ProjectID, RefreshedAdditionTicket.Generation);
		PF_CHECK(Tests, RefreshedAddition.has_value());
		if (!RefreshedAddition) return;
		const auto AdditionPublished = Catalog.Publish(RefreshedAdditionTicket, *RefreshedAddition);
		PF_CHECK(Tests, AdditionPublished.has_value());
		PF_CHECK(Tests, Catalog.GetSnapshot() == *RefreshedAddition);
		PF_CHECK(Tests, Catalog.GetSnapshot()->GetRegistry().Find(*AddedID).has_value());
		const auto AdditionSnapshot = Catalog.GetSnapshot();

		const AssetDependencyCatalogTicket RemovalTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto RemovalCandidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, RemovalTicket.Generation);
		PF_CHECK(Tests, RemovalCandidate.has_value());
		if (!RemovalCandidate) return;
		std::error_code Error;
		const std::filesystem::path AddedSource = Fixture.Absolute("Assets/Textures/LateAddition.ppm");
		const std::filesystem::path AddedSidecar = AssetMetadataSerializer::GetSidecarPath(AddedSource);
		std::filesystem::remove(AddedSource, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		std::filesystem::remove(AddedSidecar, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto RemovalRejected = Catalog.Publish(RemovalTicket, *RemovalCandidate);
		PF_CHECK(Tests, !RemovalRejected.has_value());
		if (!RemovalRejected)
		{
			PF_CHECK(Tests, IsActionable(RemovalRejected.error()));
			PF_CHECK(Tests, Mentions(RemovalRejected.error(), "registry") || Mentions(RemovalRejected.error(), "membership") ||
				Mentions(RemovalRejected.error(), "LateAddition.ppm"));
		}
		PF_CHECK(Tests, Catalog.GetSnapshot() == AdditionSnapshot);
		PF_CHECK(Tests, AdditionSnapshot->GetRegistry().Find(*AddedID).has_value());
		PF_CHECK(Tests, AdditionSnapshot->GetRegistry().Find(*ExistingID).has_value());

		const AssetDependencyCatalogTicket RefreshedRemovalTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto RefreshedRemoval = AssetDependencyCatalogBuilder::Build(
			Fixture.Root, Fixture.ProjectID, RefreshedRemovalTicket.Generation);
		PF_CHECK(Tests, RefreshedRemoval.has_value());
		if (!RefreshedRemoval) return;
		const auto RemovalPublished = Catalog.Publish(RefreshedRemovalTicket, *RefreshedRemoval);
		PF_CHECK(Tests, RemovalPublished.has_value());
		PF_CHECK(Tests, Catalog.GetSnapshot() == *RefreshedRemoval);
		PF_CHECK(Tests, !Catalog.GetSnapshot()->GetRegistry().Find(*AddedID).has_value());
	}

	void TestPublishValidatesExistingInputsAndAllowsUnchangedCandidate(TestRunner& Tests)
	{
		AssetDependencyPublicationFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const auto ExistingID = Fixture.WriteManaged("Assets/Textures/Existing.ppm", "P3\n1 1\n255\n10 20 30\n");
		PF_CHECK(Tests, ExistingID.has_value());
		if (!ExistingID) return;

		AssetDependencyCatalog Catalog;
		if (!CreatePublishedBaseline(Fixture, Catalog, *ExistingID, Tests)) return;
		const auto Retained = Catalog.GetSnapshot();
		const AssetDependencyCatalogTicket StaleTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto StaleCandidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, StaleTicket.Generation);
		PF_CHECK(Tests, StaleCandidate.has_value());
		if (!StaleCandidate) return;
		const std::filesystem::path ExistingSource = Fixture.Absolute("Assets/Textures/Existing.ppm.meta");
		const auto OriginalBytes = AssetDependencyPublicationFixture::ReadFile(ExistingSource);
		PF_CHECK(Tests, OriginalBytes.has_value());
		if (!OriginalBytes) return;
		std::string ChangedBytes = *OriginalBytes;
		const size_t VersionField = ChangedBytes.find("\"version\": 1");
		PF_CHECK(Tests, VersionField != std::string::npos);
		if (VersionField == std::string::npos) return;
		// Move one whitespace byte while keeping the sidecar valid, same-sized, and semantically identical.
		ChangedBytes.replace(VersionField, std::string("\"version\": 1").size(), "\"version\":1 ");
		std::error_code Error;
		const auto OriginalTime = std::filesystem::last_write_time(ExistingSource, Error);
		PF_CHECK(Tests, !Error);
		PF_CHECK(Tests, AssetDependencyPublicationFixture::WriteFile(ExistingSource, ChangedBytes));
		std::filesystem::last_write_time(ExistingSource, OriginalTime, Error);
		PF_CHECK(Tests, !Error);
		const auto ExistingChangeRejected = Catalog.Publish(StaleTicket, *StaleCandidate);
		PF_CHECK(Tests, !ExistingChangeRejected.has_value());
		if (!ExistingChangeRejected)
		{
			PF_CHECK(Tests, IsActionable(ExistingChangeRejected.error()));
			PF_CHECK(Tests, Mentions(ExistingChangeRejected.error(), "Existing.ppm"));
		}
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);

		PF_CHECK(Tests, AssetDependencyPublicationFixture::WriteFile(ExistingSource, *OriginalBytes));
		std::filesystem::last_write_time(ExistingSource, OriginalTime, Error);
		PF_CHECK(Tests, !Error);
		const AssetDependencyCatalogTicket FreshTicket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto FreshCandidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, FreshTicket.Generation);
		PF_CHECK(Tests, FreshCandidate.has_value());
		if (!FreshCandidate) return;
		const auto Published = Catalog.Publish(FreshTicket, *FreshCandidate);
		PF_CHECK(Tests, Published.has_value());
		PF_CHECK(Tests, Catalog.GetSnapshot() == *FreshCandidate);
		PF_CHECK(Tests, Retained->GetRegistry().Find(*ExistingID).has_value());
	}

	void TestPublishRejectsSupersededTicketDuringValidation(TestRunner& Tests)
	{
		AssetDependencyPublicationFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;
		const auto ExistingID = Fixture.WriteManaged("Assets/Textures/Existing.ppm", "P3\n1 1\n255\n10 20 30\n");
		PF_CHECK(Tests, ExistingID.has_value());
		if (!ExistingID) return;

		AssetDependencyCatalog Catalog;
		if (!CreatePublishedBaseline(Fixture, Catalog, *ExistingID, Tests)) return;
		const auto Retained = Catalog.GetSnapshot();
		const AssetDependencyCatalogTicket Ticket = Catalog.BeginRequest(Fixture.ProjectID);
		const auto Candidate = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, Ticket.Generation);
		PF_CHECK(Tests, Candidate.has_value());
		if (!Candidate) return;

		std::optional<AssetDependencyCatalogTicket> SupersedingTicket;
		AssetDependencyCatalogPublishOptions Options;
		Options.BeforeFinalTicketValidation = [&]
		{
			SupersedingTicket = Catalog.BeginRequest(Fixture.ProjectID);
		};
		const auto Rejected = Catalog.Publish(Ticket, *Candidate, Options);
		PF_CHECK(Tests, SupersedingTicket.has_value());
		PF_CHECK(Tests, !Rejected.has_value());
		if (!Rejected)
		{
			PF_CHECK(Tests, IsActionable(Rejected.error()));
			PF_CHECK(Tests, Mentions(Rejected.error(), "request") || Mentions(Rejected.error(), "supersed") ||
				Mentions(Rejected.error(), "ticket"));
		}
		PF_CHECK(Tests, Catalog.GetSnapshot() == Retained);
		if (SupersedingTicket) PF_CHECK(Tests, Catalog.IsLatestRequest(*SupersedingTicket));
	}
} // namespace

void RunAssetDependencyPublicationTests(TestRunner& Tests)
{
	TestPublishRejectsDuplicateUUIDAddedAfterBuild(Tests);
	TestPublishRejectsMalformedNewMetadata(Tests);
	TestPublishRejectsMembershipChanges(Tests);
	TestPublishValidatesExistingInputsAndAllowsUnchangedCandidate(Tests);
	TestPublishRejectsSupersededTicketDuringValidation(Tests);
}
