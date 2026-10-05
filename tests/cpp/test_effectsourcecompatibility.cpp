#include "Features/Effects11/EffectSourceCompatibility.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Interior time-of-day compatibility avoids evaluating exterior data", "[effects11]")
{
	std::string source = "#define TODIE(a) lerp( TOD(a), a##_Interior, EInteriorFactor )";

	REQUIRE(EffectSourceCompatibility::PatchInteriorTimeOfDayMacro(source));
	REQUIRE(source == "#define TODIE(a) (EInteriorFactor ? a##_Interior : TOD(a))");
}

TEST_CASE("Interior time-of-day compatibility leaves other source unchanged", "[effects11]")
{
	std::string source = "#define TODIE(a) lerp(TOD(a), a##_Interior, EInteriorFactor)";

	REQUIRE_FALSE(EffectSourceCompatibility::PatchInteriorTimeOfDayMacro(source));
	REQUIRE(source == "#define TODIE(a) lerp(TOD(a), a##_Interior, EInteriorFactor)");
}
