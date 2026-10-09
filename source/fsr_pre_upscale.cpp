/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "fsr_pre_upscale.hpp"
#include "hook_manager.hpp"
#include "dll_log.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <string>
#include <vector>

namespace
{
	using ffx_fsr3_dispatch_fn = int (*)(void *context, void *dispatch_description);

	std::atomic_flag s_scan_in_progress = ATOMIC_FLAG_INIT;
	std::atomic_flag s_logged_dispatch = ATOMIC_FLAG_INIT;
	std::vector<void *> s_hooked_dispatches;

	int ffx_fsr3_dispatch(void *context, void *dispatch_description)
	{
		// The FSR 3.1 dispatch ABI is (context, dispatch-description). Keep the
		// arguments opaque here so this hook never assumes a mismatched SDK layout.
		// The actual dispatch must run exactly once and retain its original return code.
		if (!s_logged_dispatch.test_and_set())
		{
			reshade::log::message(
				reshade::log::level::info,
				"FSR pre-upscale: intercepted ffxFsr3UpscalerContextDispatch (context=%p, description=%p). Forwarding unchanged; effect execution is not enabled yet.",
				context, dispatch_description);
		}

		const auto original = reshade::hooks::call<ffx_fsr3_dispatch_fn>(
			reinterpret_cast<ffx_fsr3_dispatch_fn>(&ffx_fsr3_dispatch));
		if (original == nullptr)
			return -1;

		return original(context, dispatch_description);
	}

	void inspect_module(HMODULE module)
	{
		if (module == nullptr)
			return;

		const auto target = reinterpret_cast<void *>(
			GetProcAddress(module, "ffxFsr3UpscalerContextDispatch"));
		if (target == nullptr || target == reinterpret_cast<void *>(&ffx_fsr3_dispatch))
			return;

		for (const void *hooked : s_hooked_dispatches)
			if (hooked == target)
				return;

		if (reshade::hooks::install(
			"ffxFsr3UpscalerContextDispatch",
			target,
			reinterpret_cast<void *>(&ffx_fsr3_dispatch)))
		{
			s_hooked_dispatches.push_back(target);
			reshade::log::message(
				reshade::log::level::info,
				"FSR pre-upscale: hooked FSR 3.1 dispatch export in module %p.",
				module);
		}
	}

	void scan_loaded_modules()
	{
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
		if (snapshot == INVALID_HANDLE_VALUE)
			return;

		MODULEENTRY32W entry = {};
		entry.dwSize = sizeof(entry);
		if (Module32FirstW(snapshot, &entry))
		{
			do
			{
				inspect_module(entry.hModule);
			} while (Module32NextW(snapshot, &entry));
		}

		CloseHandle(snapshot);
	}
}

void reshade::fsr_pre_upscale::update()
{
	if (s_scan_in_progress.test_and_set())
		return;

	scan_loaded_modules();
	s_scan_in_progress.clear();
}
