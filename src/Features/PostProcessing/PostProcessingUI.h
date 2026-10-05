#pragma once

#include <array>
#include <imgui.h>

namespace PostProcessingUI
{
	bool ActionButton(const char* label);
	bool IconButton(const char* icon);
	bool FFTResolutionCombo(const char* label, int& resolution);
	bool LabeledSliderFloat3(
		const char* label,
		float* values,
		const std::array<const char*, 3>& componentLabels,
		float min,
		float max,
		const char* format = "%.3f",
		ImGuiSliderFlags flags = 0);
	bool RGBFloatDrag3(const char* label, float* values, float speed, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
	bool RGBFloatDrag3(const char* label, float* values, float speed, float min, float max, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
}
