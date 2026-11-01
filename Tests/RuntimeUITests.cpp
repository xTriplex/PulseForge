#include "TestSupport.h"

#include "UI/RuntimeUI.h"

#include <array>

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
