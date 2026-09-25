#pragma once

#include <string>

namespace EffectSourceCompatibility
{
	/// @brief Prevents interior-only values from evaluating undefined exterior time-of-day data.
	/// @return True when the known compatibility pattern was replaced.
	bool PatchInteriorTimeOfDayMacro(std::string& a_source);
}
