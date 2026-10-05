#pragma once

namespace Util
{
	/**
	 * @brief Retrieve the file version of a DLL using the Win32 version-info API.
	 * @param dllPath Absolute path to the DLL file.
	 * @return The parsed version, or std::nullopt if version info is unavailable.
	 */
	std::optional<REL::Version> GetDllVersion(const std::wstring& dllPath);

	/**
	 * @brief Get the number of logical processors on the highest-efficiency cores.
	 *
	 * On Intel hybrid CPUs this returns only P-core logical processors.
	 * On non-hybrid CPUs all cores share the same efficiency class, so this
	 * returns std::thread::hardware_concurrency(). Falls back to
	 * hardware_concurrency() on any API failure. The result is cached.
	 *
	 * @return The logical processor count for performance cores.
	 */
	uint32_t GetPerformanceCoreCount();

	/**
	 * @brief Wine/Proton version string if running under Wine, std::nullopt on native Windows.
	 *
	 * Uses ntdll's wine_get_version export, which only Wine provides.
	 */
	std::optional<std::string> GetWineVersion();

	/**
	 * @brief True if a loaded module is one of Wine's built-in DLL implementations
	 * (as opposed to a native Microsoft DLL installed into the prefix).
	 *
	 * Wine stamps "Wine builtin DLL" into the DOS header area of its built-in PE modules.
	 */
	bool IsWineBuiltinModule(HMODULE a_module);
}  // namespace Util
