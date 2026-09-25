#include "EffectSourceCompatibility.h"

#include <string_view>

namespace EffectSourceCompatibility
{
	bool PatchInteriorTimeOfDayMacro(std::string& a_source)
	{
		constexpr std::string_view unsafeMacro = "lerp( TOD(a), a##_Interior, EInteriorFactor )";
		constexpr std::string_view guardedMacro = "(EInteriorFactor ? a##_Interior : TOD(a))";

		const auto position = a_source.find(unsafeMacro);
		if (position == std::string::npos)
			return false;

		a_source.replace(position, unsafeMacro.size(), guardedMacro);
		return true;
	}
}
