#include "TestSupport.h"

#include "UI/RuntimeUI.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
	PulseForge::RuntimeUIElement MakeButton(uint64_t ID, int32_t Order, glm::vec2 Minimum, glm::vec2 Maximum, std::string Action)
	{
		PulseForge::RuntimeUIElement Element;
		Element.ID = PulseForge::UUID{ 0, ID };
		Element.Kind = PulseForge::RuntimeUIElementKind::Button;
		Element.Order = Order;
		Element.OffsetMinimum = Minimum;
		Element.OffsetMaximum = Maximum;
		Element.Action = std::move(Action);
		return Element;
	}
}

void RunRuntimeUITests(TestRunner& Tests)
{
	using namespace PulseForge;
	RuntimeUIDocument Document;
	Document.Elements.push_back(MakeButton(1, 0, { 100.0f, 80.0f }, { 300.0f, 140.0f }, "start"));
	Document.Elements.push_back(MakeButton(2, 1, { 100.0f, 80.0f }, { 300.0f, 140.0f }, "covered"));
	Document.Elements[1].Order = 1;
	Document.Elements[0].Order = 0;
	Document.Elements[0].OffsetMaximum = { 300.0f, 140.0f };
	Document.Elements[1].OffsetMinimum = { 140.0f, 100.0f };
	Document.Elements[1].OffsetMaximum = { 340.0f, 160.0f };

	const RuntimeUIViewport Viewport{ .LogicalSize = { 1920.0f, 1080.0f }, .FramebufferWidth = 3840, .FramebufferHeight = 2160 };
	auto Layout = ComputeRuntimeUILayout(Document, Viewport);
	PF_CHECK(Tests, Layout.has_value());
	if (!Layout) return;
	PF_CHECK(Tests, (*Layout)[0].Bounds.Left == 200.0f && (*Layout)[0].Bounds.Top == 160.0f);
	PF_CHECK(Tests, (*Layout)[0].Bounds.Right == 600.0f && (*Layout)[0].Bounds.Bottom == 280.0f);
	PF_CHECK(Tests, HitTestRuntimeUI(Document, *Layout, { 300.0f, 220.0f }) == UUID(0, 2));

	RuntimeUIController Controller;
	auto FocusLayout = Controller.Layout(Document, Viewport, 10);
	PF_CHECK(Tests, FocusLayout.has_value() && Controller.GetFocusedElement() == UUID(0, 1));
	const std::array PressRelease{
		RuntimeUIInputEvent{ .Kind = RuntimeUIInputKind::PointerPressed, .Position = { 300.0f, 220.0f } },
		RuntimeUIInputEvent{ .Kind = RuntimeUIInputKind::PointerReleased, .Position = { 300.0f, 220.0f } }
	};
	auto Actions = Controller.ProcessInput(Document, *FocusLayout, PressRelease, 10);
	PF_CHECK(Tests, Actions.size() == 1 && Actions[0].Element == UUID(0, 2) && Actions[0].Name == "covered");
	PF_CHECK(Tests, Actions[0].TreeGeneration == 10);

	Document.Elements[1].Visible = false;
	FocusLayout = Controller.Layout(Document, Viewport, 10);
	PF_CHECK(Tests, FocusLayout.has_value() && Controller.GetFocusedElement() == UUID(0, 1));
	const std::array Activate{ RuntimeUIInputEvent{ .Kind = RuntimeUIInputKind::Activate } };
	Actions = Controller.ProcessInput(Document, *FocusLayout, Activate, 10);
	PF_CHECK(Tests, Actions.size() == 1 && Actions[0].Name == "start");
	const std::array Press{ RuntimeUIInputEvent{ .Kind = RuntimeUIInputKind::PointerPressed, .Position = { 300.0f, 220.0f } } };
	(void)Controller.ProcessInput(Document, *FocusLayout, Press, 10);
	Document.Elements[0].Visible = false;
	FocusLayout = Controller.Layout(Document, Viewport, 10);
	PF_CHECK(Tests, FocusLayout.has_value());
	const std::array Release{ RuntimeUIInputEvent{ .Kind = RuntimeUIInputKind::PointerReleased, .Position = { 300.0f, 220.0f } } };
	Actions = Controller.ProcessInput(Document, *FocusLayout, Release, 10);
	PF_CHECK(Tests, Actions.empty());

	RuntimeUIDocument Navigation;
	Navigation.Elements.push_back(MakeButton(10, 0, { 20.0f, 20.0f }, { 80.0f, 70.0f }, "current"));
	Navigation.Elements.push_back(MakeButton(11, 1, { 20.0f, 140.0f }, { 80.0f, 190.0f }, "below"));
	Navigation.Elements.push_back(MakeButton(12, 2, { 140.0f, 20.0f }, { 200.0f, 70.0f }, "right"));
	RuntimeUIController NavigationController;
	auto NavigationLayout = NavigationController.Layout(Navigation, Viewport, 11);
	PF_CHECK(Tests, NavigationLayout.has_value() && NavigationController.GetFocusedElement() == UUID(0, 10));
	const std::array NavigateRight{ RuntimeUIInputEvent{ .Kind = RuntimeUIInputKind::Navigate, .Direction = { 1.0f, 0.0f } } };
	(void)NavigationController.ProcessInput(Navigation, *NavigationLayout, NavigateRight, 11);
	PF_CHECK(Tests, NavigationController.GetFocusedElement() == UUID(0, 12));
	const std::array FocusLost{ RuntimeUIInputEvent{ .Kind = RuntimeUIInputKind::FocusLost } };
	(void)NavigationController.ProcessInput(Navigation, *NavigationLayout, FocusLost, 11);
	PF_CHECK(Tests, !NavigationController.GetHoveredElement());
	NavigationController.Reset(11);
	NavigationLayout = NavigationController.Layout(Navigation, Viewport, 11);

	RuntimeUIInputAdapter Adapter;
	RuntimeUIHostInputSample Host{
		.TreeGeneration = 11, .InputOwner = 5, .Viewport = Viewport, .DeltaSeconds = 1.0 / 60.0,
		.Focused = true, .PointerPosition = glm::vec2{ 40.0f, 40.0f }
	};
	auto HostEvents = Adapter.BuildEvents(Host);
	(void)NavigationController.ProcessInput(Navigation, *NavigationLayout, HostEvents, 11);
	PF_CHECK(Tests, NavigationController.GetHoveredElement() == UUID(0, 10));
	PF_CHECK(Tests, NavigationController.GetFocusedElement() == UUID(0, 10));
	Host.Navigation = { 1.0f, 0.0f };
	Host.ActivateHeld = true;
	HostEvents = Adapter.BuildEvents(Host);
	(void)NavigationController.ProcessInput(Navigation, *NavigationLayout, HostEvents, 11);
	PF_CHECK(Tests, NavigationController.GetFocusedElement() == UUID(0, 12));
	for (size_t Frame = 0; Frame < 8; ++Frame)
	{
		Host.DeltaSeconds = 1.0 / 60.0;
		HostEvents = Adapter.BuildEvents(Host);
		Actions = NavigationController.ProcessInput(Navigation, *NavigationLayout, HostEvents, 11);
		PF_CHECK(Tests, NavigationController.GetHoveredElement() == UUID(0, 10));
		PF_CHECK(Tests, NavigationController.GetFocusedElement() == UUID(0, 12));
	}
	Host.ActivatePressed = true;
	Host.ActivateHeld = true;
	HostEvents = Adapter.BuildEvents(Host);
	Actions = NavigationController.ProcessInput(Navigation, *NavigationLayout, HostEvents, 11);
	PF_CHECK(Tests, Actions.size() == 1 && Actions[0].Element == UUID(0, 12) && Actions[0].Name == "right");
	Host.ActivatePressed = false;
	Host.ActivateHeld = false;
	Host.Navigation = {};
	Host.PointerPosition = glm::vec2{ 40.0f, 40.0f };
	Host.PointerPressed = true;
	Host.PointerHeld = true;
	HostEvents = Adapter.BuildEvents(Host);
	(void)NavigationController.ProcessInput(Navigation, *NavigationLayout, HostEvents, 11);
	PF_CHECK(Tests, NavigationController.GetFocusedElement() == UUID(0, 10));
	Host.PointerPressed = false;
	Host.PointerHeld = false;
	Host.PointerReleased = true;
	HostEvents = Adapter.BuildEvents(Host);
	Actions = NavigationController.ProcessInput(Navigation, *NavigationLayout, HostEvents, 11);
	PF_CHECK(Tests, Actions.size() == 1 && Actions[0].Element == UUID(0, 10) && Actions[0].Name == "current");

	RuntimeUIInputAdapter RepeatAdapter;
	RuntimeUIHostInputSample Repeat{ .TreeGeneration = 21, .InputOwner = 8,
		.Viewport = { { 300.0f, 200.0f }, 450, 300 }, .DeltaSeconds = 1.0 / 120.0, .Focused = true };
	auto RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, RepeatEvents.empty());
	Repeat.Navigation = { 1.0f, 0.0f };
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, std::count_if(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::Navigate; }) == 1);
	auto CountNavigation = [](double FrameDelta, size_t FrameCount)
	{
		RuntimeUIInputAdapter TimedAdapter;
		RuntimeUIHostInputSample Sample{ .TreeGeneration = 30, .InputOwner = 9,
			.Viewport = { { 300.0f, 200.0f }, 450, 300 }, .Focused = true };
		(void)TimedAdapter.BuildEvents(Sample);
		Sample.Navigation = { 1.0f, 0.0f };
		size_t Count = 0;
		for (size_t Index = 0; Index < FrameCount; ++Index)
		{
			Sample.DeltaSeconds = FrameDelta;
			const auto Events = TimedAdapter.BuildEvents(Sample);
			Count += static_cast<size_t>(std::count_if(Events.begin(), Events.end(),
				[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::Navigate; }));
		}
		return Count;
	};
	const size_t RepeatAt120Hz = CountNavigation(1.0 / 120.0, 96);
	const size_t RepeatAt30Hz = CountNavigation(1.0 / 30.0, 24);
	PF_CHECK(Tests, RepeatAt120Hz == RepeatAt30Hz && RepeatAt120Hz >= 3 && RepeatAt120Hz <= 5);
	Repeat.Navigation = { -1.0f, 0.0f };
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, std::count_if(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::Navigate && Event.Direction.x < 0.0f; }) == 1);
	Repeat.Navigation = { 0.34f, 0.0f };
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, std::none_of(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::Navigate; }));
	Repeat.Focused = false;
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, RepeatEvents.size() == 1 && RepeatEvents[0].Kind == RuntimeUIInputKind::FocusLost);
	Repeat.Focused = true;
	Repeat.Navigation = {};
	Repeat.ActivateHeld = true;
	Repeat.ActivatePressed = true;
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, std::none_of(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::Activate; }));
	Repeat.ActivateHeld = false;
	Repeat.ActivatePressed = false;
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	Repeat.ActivateHeld = true;
	Repeat.ActivatePressed = true;
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, std::count_if(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::Activate; }) == 1);
	Repeat.ActivateHeld = false;
	Repeat.ActivatePressed = false;
	Repeat.PointerPosition = glm::vec2{ 20.0f, 20.0f };
	(void)RepeatAdapter.BuildEvents(Repeat);
	Repeat.ViewportOrigin = { 160.0f, 90.0f };
	Repeat.PointerPosition = glm::vec2{ 25.0f, 20.0f };
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, std::any_of(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::CaptureCancelled; }));
	PF_CHECK(Tests, std::none_of(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::PointerMoved; }));
	Repeat.ViewportOrigin = {};
	(void)RepeatAdapter.BuildEvents(Repeat);
	Repeat.InputOwner = 10;
	Repeat.PointerPosition = glm::vec2{ 25.0f, 20.0f };
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, std::any_of(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::CaptureCancelled; }));
	PF_CHECK(Tests, std::none_of(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::PointerMoved; }));
	Repeat.InputOwner = 11;
	Repeat.TextInputBlocked = true;
	Repeat.ActivateHeld = true;
	Repeat.ActivatePressed = true;
	RepeatEvents = RepeatAdapter.BuildEvents(Repeat);
	PF_CHECK(Tests, std::none_of(RepeatEvents.begin(), RepeatEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::Activate; }));

	RuntimeUIDocument CapturedPointerDocument;
	CapturedPointerDocument.Elements.push_back(MakeButton(3100, 0, { 0.0f, 0.0f }, { 100.0f, 100.0f }, "captured"));
	RuntimeUIController CapturedPointerController;
	auto CapturedPointerLayout = CapturedPointerController.Layout(CapturedPointerDocument, Viewport, 31);
	RuntimeUIInputAdapter CapturedPointerAdapter;
	RuntimeUIHostInputSample CapturedPointerHost{
		.SessionGeneration = 41, .TreeGeneration = 31, .InputOwner = 12, .Viewport = Viewport, .Focused = true,
		.PointerPosition = glm::vec2{ 20.0f, 20.0f }
	};
	auto CapturedPointerEvents = CapturedPointerAdapter.BuildEvents(CapturedPointerHost);
	(void)CapturedPointerController.ProcessInput(CapturedPointerDocument, *CapturedPointerLayout, CapturedPointerEvents, 31);
	CapturedPointerHost.PointerPressed = true;
	CapturedPointerHost.PointerHeld = true;
	CapturedPointerEvents = CapturedPointerAdapter.BuildEvents(CapturedPointerHost);
	(void)CapturedPointerController.ProcessInput(CapturedPointerDocument, *CapturedPointerLayout, CapturedPointerEvents, 31);
	PF_CHECK(Tests, CapturedPointerController.GetFocusedElement() == UUID(0, 3100));
	CapturedPointerHost.SessionGeneration = 42;
	CapturedPointerEvents = CapturedPointerAdapter.BuildEvents(CapturedPointerHost);
	PF_CHECK(Tests, std::any_of(CapturedPointerEvents.begin(), CapturedPointerEvents.end(),
		[](const RuntimeUIInputEvent& Event) { return Event.Kind == RuntimeUIInputKind::CaptureCancelled; }));
	(void)CapturedPointerController.ProcessInput(
		CapturedPointerDocument, *CapturedPointerLayout, CapturedPointerEvents, 31);
	CapturedPointerHost.PointerHeld = false;
	CapturedPointerHost.PointerPressed = false;
	CapturedPointerHost.PointerReleased = true;
	CapturedPointerEvents = CapturedPointerAdapter.BuildEvents(CapturedPointerHost);
	const auto CapturedPointerActions = CapturedPointerController.ProcessInput(
		CapturedPointerDocument, *CapturedPointerLayout, CapturedPointerEvents, 31);
	PF_CHECK(Tests, CapturedPointerActions.empty());

	GameplayActionMap InputMap;
	const InputControl SharedKey{ .Device = InputDevice::Keyboard, .Code = static_cast<uint16_t>(InputKey::Space) };
	for (const auto& [Name, Context] : { std::pair{ "game.fire", InputContext::Gameplay },
		std::pair{ "ui.activate", InputContext::RuntimeUI } })
	{
		GameplayActionDefinition Action;
		Action.Id = Name;
		Action.Type = ActionValueType::Digital;
		Action.Context = Context;
		Action.Bindings.push_back({ .Id = std::string(Name) + ".space", .Kind = ActionBindingKind::Button,
			.Primary = SharedKey, .ScaleX = 1.0f });
		InputMap.Actions.push_back(std::move(Action));
	}
	const auto MakeRouting = [](RuntimeUIInputPolicy Policy)
	{
		InputRoutingState Result;
		Result.FocusedWindow = 41;
		Result.Contexts[Result.ContextCount++] = { .Context = InputContext::Gameplay,
			.Consumes = InputDeviceMask::None, .OwnerWindow = 41, .Active = true };
		(void)AppendRuntimeUIInputContext(Result, Policy, 41);
		return Result;
	};
	auto RoutedInput = [&](RuntimeUIInputPolicy Policy)
	{
		GameplayInputService Service;
		PF_CHECK(Tests, Service.SetActionMap(InputMap).has_value());
		PF_CHECK(Tests, Service.QueueEvent({ .Kind = InputEventKind::Pressed, .Control = SharedKey, .Window = 41 }));
		Service.PublishFrame(MakeRouting(Policy));
		return std::pair{ Service.FindAction("game.fire") ? Service.FindAction("game.fire")->Pressed : false,
			Service.FindAction("ui.activate") ? Service.FindAction("ui.activate")->Pressed : false };
	};
	PF_CHECK(Tests, RoutedInput(RuntimeUIInputPolicy::Passive) == (std::pair{ true, false }));
	PF_CHECK(Tests, RoutedInput(RuntimeUIInputPolicy::NonModal) == (std::pair{ true, true }));
	PF_CHECK(Tests, RoutedInput(RuntimeUIInputPolicy::Modal) == (std::pair{ false, true }));
	GameplayInputService QuarantineInput;
	PF_CHECK(Tests, QuarantineInput.SetActionMap(InputMap).has_value());
	PF_CHECK(Tests, QuarantineInput.QueueEvent({ .Kind = InputEventKind::Pressed, .Control = SharedKey, .Window = 41 }));
	QuarantineInput.PublishFrame(MakeRouting(RuntimeUIInputPolicy::Modal));
	QuarantineInput.PublishFrame(MakeRouting(RuntimeUIInputPolicy::NonModal));
	PF_CHECK(Tests, QuarantineInput.FindAction("game.fire") && !QuarantineInput.FindAction("game.fire")->Pressed);
	PF_CHECK(Tests, QuarantineInput.QueueEvent({ .Kind = InputEventKind::Released, .Control = SharedKey, .Window = 41 }));
	QuarantineInput.PublishFrame(MakeRouting(RuntimeUIInputPolicy::NonModal));
	PF_CHECK(Tests, QuarantineInput.QueueEvent({ .Kind = InputEventKind::Pressed, .Control = SharedKey, .Window = 41 }));
	QuarantineInput.PublishFrame(MakeRouting(RuntimeUIInputPolicy::NonModal));
	PF_CHECK(Tests, QuarantineInput.FindAction("game.fire") && QuarantineInput.FindAction("game.fire")->Pressed);

	RuntimeUIDocument Clipped;
	RuntimeUIElement Parent;
	Parent.ID = UUID{ 0, 20 };
	Parent.Kind = RuntimeUIElementKind::Image;
	Parent.Image = UUID{ 0, 22 };
	Parent.OffsetMinimum = { 0.0f, 0.0f };
	Parent.OffsetMaximum = { 100.0f, 100.0f };
	Parent.ClipChildren = true;
	Clipped.Elements.push_back(Parent);
	RuntimeUIElement Child = MakeButton(21, 0, { 80.0f, 20.0f }, { 150.0f, 80.0f }, "clipped");
	Child.Parent = Parent.ID;
	Clipped.Elements.push_back(Child);
	auto ClippedLayout = ComputeRuntimeUILayout(Clipped, { { 960.0f, 540.0f }, 1440, 810 });
	PF_CHECK(Tests, ClippedLayout.has_value());
	if (ClippedLayout)
	{
		PF_CHECK(Tests, ClippedLayout->at(1).Clip.Right == 100.0f);
		PF_CHECK(Tests, HitTestRuntimeUI(Clipped, *ClippedLayout, { 120.0f, 50.0f }) == std::nullopt);
		PF_CHECK(Tests, HitTestRuntimeUI(Clipped, *ClippedLayout, { 90.0f, 50.0f }) == UUID(0, 21));
	}

	const auto MakeOrderedChain = [](size_t Count, const std::vector<size_t>& LogicalOrder)
	{
		RuntimeUIDocument Chain;
		Chain.Elements.reserve(Count);
		std::vector<size_t> Indices(Count);
		for (const size_t Logical : LogicalOrder)
		{
			Indices[Logical] = Chain.Elements.size();
			Chain.Elements.push_back(MakeButton(1000 + Logical, static_cast<int32_t>(Logical),
				{ 0.0f, 0.0f }, { 100.0f, 100.0f }, "node"));
		}
		for (size_t Logical = 1; Logical < Count; ++Logical)
			Chain.Elements[Indices[Logical]].Parent = Chain.Elements[Indices[Logical - 1]].ID;
		return Chain;
	};
	std::vector<size_t> ParentFirst(RuntimeUIDocument::MaximumHierarchyDepth);
	for (size_t Index = 0; Index < ParentFirst.size(); ++Index) ParentFirst[Index] = Index;
	std::vector<size_t> ChildFirst(ParentFirst.rbegin(), ParentFirst.rend());
	std::vector<size_t> Shuffled;
	for (size_t Index = 0; Index < ParentFirst.size(); ++Index) Shuffled.push_back((Index * 13) % ParentFirst.size());
	for (const auto& Order : { ParentFirst, ChildFirst, Shuffled })
	{
		RuntimeUIDocument Chain = MakeOrderedChain(RuntimeUIDocument::MaximumHierarchyDepth, Order);
		PF_CHECK(Tests, ValidateRuntimeUIDocument(Chain).has_value());
		auto ChainLayout = ComputeRuntimeUILayout(Chain, { { 400.0f, 300.0f }, 600, 450 });
		PF_CHECK(Tests, ChainLayout.has_value());
		if (ChainLayout)
			PF_CHECK(Tests, std::all_of(ChainLayout->begin(), ChainLayout->end(),
				[](const RuntimeUILayoutEntry& Entry) { return Entry.Visible; }));
	}
	std::vector<size_t> OneTooDeep(RuntimeUIDocument::MaximumHierarchyDepth + 1);
	for (size_t Index = 0; Index < OneTooDeep.size(); ++Index) OneTooDeep[Index] = Index;
	std::vector<size_t> OneTooDeepReversed(OneTooDeep.rbegin(), OneTooDeep.rend());
	std::vector<size_t> OneTooDeepShuffled;
	for (size_t Index = 0; Index < OneTooDeep.size(); ++Index) OneTooDeepShuffled.push_back((Index * 17) % OneTooDeep.size());
	for (const auto& Order : { OneTooDeep, OneTooDeepReversed, OneTooDeepShuffled })
		PF_CHECK(Tests, !ValidateRuntimeUIDocument(MakeOrderedChain(OneTooDeep.size(), Order)).has_value());
	std::vector<size_t> ExcessiveOrder(512);
	for (size_t Index = 0; Index < ExcessiveOrder.size(); ++Index) ExcessiveOrder[Index] = (Index * 127) % ExcessiveOrder.size();
	PF_CHECK(Tests, !ValidateRuntimeUIDocument(MakeOrderedChain(ExcessiveOrder.size(), ExcessiveOrder)).has_value());
	RuntimeUIDocument MissingParent;
	MissingParent.Elements.push_back(MakeButton(2000, 0, { 0.0f, 0.0f }, { 50.0f, 50.0f }, "missing"));
	MissingParent.Elements[0].Parent = UUID{ 0, 9999 };
	PF_CHECK(Tests, !ValidateRuntimeUIDocument(MissingParent));
	RuntimeUIDocument SelfCycle;
	SelfCycle.Elements.push_back(MakeButton(2001, 0, { 0.0f, 0.0f }, { 50.0f, 50.0f }, "self"));
	SelfCycle.Elements[0].Parent = SelfCycle.Elements[0].ID;
	PF_CHECK(Tests, !ValidateRuntimeUIDocument(SelfCycle));
	RuntimeUIDocument SharedAncestor;
	SharedAncestor.Elements = { MakeButton(2010, 0, { 0.0f, 0.0f }, { 100.0f, 100.0f }, "root"),
		MakeButton(2011, 1, { 0.0f, 0.0f }, { 50.0f, 50.0f }, "left"),
		MakeButton(2012, 2, { 50.0f, 0.0f }, { 100.0f, 50.0f }, "right"),
		MakeButton(2013, 0, { 0.0f, 0.0f }, { 50.0f, 50.0f }, "nested") };
	SharedAncestor.Elements[1].Parent = SharedAncestor.Elements[0].ID;
	SharedAncestor.Elements[2].Parent = SharedAncestor.Elements[0].ID;
	SharedAncestor.Elements[3].Parent = SharedAncestor.Elements[1].ID;
	PF_CHECK(Tests, ValidateRuntimeUIDocument(SharedAncestor).has_value());
	SharedAncestor.Elements[0].Parent = SharedAncestor.Elements[3].ID;
	PF_CHECK(Tests, !ValidateRuntimeUIDocument(SharedAncestor));

	Document.Elements[0].Parent = Document.Elements[1].ID;
	Document.Elements[1].Parent = Document.Elements[0].ID;
	PF_CHECK(Tests, !ValidateRuntimeUIDocument(Document));

	Document = {};
	Document.Elements.push_back(MakeButton(3, 0, { 10.0f, 10.0f }, { 50.0f, 50.0f }, "unicode"));
	Document.Elements[0].Font = UUID{ 0, 50 };
	Document.Elements[0].Text = "Caf\xC3\xA9";
	auto Serialized = SerializeRuntimeUIDocument(Document);
	PF_CHECK(Tests, Serialized.has_value());
	if (Serialized)
	{
		auto Parsed = ParseRuntimeUIDocument(*Serialized);
		PF_CHECK(Tests, Parsed.has_value() && Parsed->Elements[0].Text == Document.Elements[0].Text);
	}
	Document.Elements[0].Text = "e\xCC\x81";
	PF_CHECK(Tests, ValidateRuntimeUIDocument(Document).has_value());
	Document.Elements[0].Text = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";
	PF_CHECK(Tests, ValidateRuntimeUIDocument(Document).has_value());
	Document.Elements[0].Text = "bad\xC3";
	PF_CHECK(Tests, !ValidateRuntimeUIDocument(Document));
}
