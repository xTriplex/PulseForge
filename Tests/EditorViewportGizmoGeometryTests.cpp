#include "TestSupport.h"
#include "Editor/ViewportGizmoGeometry.h"

#include <array>
#include <limits>

namespace
{
	void TestViewportGizmoGeometry(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		ViewportGizmoGeometryInput Input;
		Input.ViewProjection = glm::mat4(1.0f);
		Input.ViewProjection[0][0] = 0.25f;
		Input.ViewProjection[1][1] = 0.25f;
		Input.ViewProjection[2][2] = 0.1f;
		Input.CameraPosition = glm::vec3(0.0f, 0.0f, 4.0f);
		Input.CameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
		Input.EntityWorldPosition = glm::vec3(0.0f, 0.0f, 0.5f);
		Input.ImageRect = { glm::vec2(100.0f, 50.0f), glm::vec2(900.0f, 650.0f) };

		const auto Geometry = BuildViewportGizmoGeometry(Input);
		PF_CHECK(Tests, Geometry.has_value());
		if (!Geometry) return;
		PF_CHECK(Tests, Geometry->IsValid);
		PF_CHECK(Tests, glm::length(Geometry->PivotWorld - Input.EntityWorldPosition) < 0.0001f);
		PF_CHECK(Tests, glm::length(Geometry->PivotScreen - glm::vec2(500.0f, 350.0f)) < 0.001f);
		PF_CHECK(Tests, Geometry->Axes[0].IsValid);
		PF_CHECK(Tests, glm::length(Geometry->Axes[0].Endpoint - Geometry->PivotScreen) >= 14.0f);

		const glm::vec2 AxisHit = (Geometry->Axes[0].Endpoint + Geometry->PivotScreen) * 0.5f;
		const auto TranslationHandle = FindViewportGizmoHandle(*Geometry, TransformGizmoOperation::Translate, AxisHit);
		PF_CHECK(Tests, TranslationHandle.has_value());
		if (TranslationHandle)
		{
			PF_CHECK(Tests, TranslationHandle->Operation == TransformGizmoOperation::Translate);
			PF_CHECK(Tests, TranslationHandle->Axis == 0);
		}
		const auto ScaleHandle = FindViewportGizmoHandle(*Geometry, TransformGizmoOperation::Scale, AxisHit);
		PF_CHECK(Tests, ScaleHandle && ScaleHandle->Operation == TransformGizmoOperation::Scale && ScaleHandle->Axis == 0);

		PF_CHECK(Tests, Geometry->RotationRingValid[0]);
		const auto RotationHandle = FindViewportGizmoHandle(*Geometry, TransformGizmoOperation::Rotate, Geometry->RotationRings[0][6]);
		PF_CHECK(Tests, RotationHandle.has_value());
		PF_CHECK(Tests, RotationHandle && RotationHandle->Operation == TransformGizmoOperation::Rotate);
		PF_CHECK(Tests, !FindViewportGizmoHandle(
							*Geometry, TransformGizmoOperation::Translate, glm::vec2(std::numeric_limits<float>::quiet_NaN(), 0.0f)));

		const std::array<glm::vec3, 3> SelectedPositions{ glm::vec3(1.0f, 2.0f, 0.5f), glm::vec3(3.0f, 4.0f, 0.5f),
			glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.5f) };
		Input.SelectedRootWorldPositions = SelectedPositions;
		Input.HasMultipleSelectedRoots = true;
		const auto MultiSelectionGeometry = BuildViewportGizmoGeometry(Input);
		PF_CHECK(Tests, MultiSelectionGeometry.has_value());
		if (MultiSelectionGeometry)
			PF_CHECK(Tests, glm::length(MultiSelectionGeometry->PivotWorld - glm::vec3(2.0f, 3.0f, 0.5f)) < 0.0001f);

		Input.ViewProjection[0][0] = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !BuildViewportGizmoGeometry(Input));
		Input.ViewProjection = glm::mat4(1.0f);
		Input.ImageRect.Maximum.y = Input.ImageRect.Minimum.y;
		PF_CHECK(Tests, !BuildViewportGizmoGeometry(Input));
	}
} // namespace

void RunEditorViewportGizmoGeometryTests(TestRunner& Tests)
{
	TestViewportGizmoGeometry(Tests);
}
