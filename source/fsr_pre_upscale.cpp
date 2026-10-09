/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "fsr_pre_upscale.hpp"
#include "hook_manager.hpp"
#include "dll_log.hpp"
#include "runtime_manager.hpp"
#include <d3d12.h>

#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <string>
#include <vector>

namespace
{
	using ffx_fsr3_dispatch_fn = int (*)(void *context, void *dispatch_description);

	std::atomic_flag s_scan_in_progress = ATOMIC_FLAG_INIT;
	std::atomic<ULONGLONG> s_last_scan_tick = 0;
	std::atomic_flag s_logged_dispatch = ATOMIC_FLAG_INIT;
	std::atomic_flag s_logged_ffx_dispatch = ATOMIC_FLAG_INIT;
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
				"FSR pre-upscale: intercepted ffxFsr3UpscalerContextDispatch (context=%p, description=%p). forwarding dispatch after pre-upscale resource attempt.",
				context, dispatch_description);
		}

		// FSR 3.1's dispatch description begins with commandList followed by the color
		// FfxResource. Its first field is the native resource pointer. Execute ReShade
		// on that render-resolution input before the original upscaler consumes it.
		struct fsr31_dispatch_prefix
		{
			void *command_list;
			struct { void *resource; } color;
		};
		if (dispatch_description != nullptr)
		{
			const auto *dispatch = static_cast<const fsr31_dispatch_prefix *>(dispatch_description);
			if (dispatch->command_list != nullptr && dispatch->color.resource != nullptr &&
				reshade::render_effects_on_external_d3d12_resource(dispatch->command_list, dispatch->color.resource))
				reshade::log::message(reshade::log::level::info, "FSR pre-upscale: ReShade effects recorded on FSR 3.1 input color before dispatch.");
		}

		const auto original = reshade::hooks::call<ffx_fsr3_dispatch_fn>(
			reinterpret_cast<ffx_fsr3_dispatch_fn>(&ffx_fsr3_dispatch));
		if (original == nullptr)
			return -1;

		return original(context, dispatch_description);
	}

	int ffx_api_dispatch(void *dispatch_header)
	{
		// The unified FFX API is used by newer integrations, including some FSR 4
		// paths. Keep the header opaque and forward it without changing its chain.
		if (!s_logged_ffx_dispatch.test_and_set())
		{
			reshade::log::message(
				reshade::log::level::info,
				"FSR pre-upscale: intercepted ffxDispatch (header=%p). Forwarding unchanged; effect execution is not enabled yet.",
				dispatch_header);
		}

		const auto original = reshade::hooks::call< int (*)(void *) >(
			reinterpret_cast< int (*)(void *) >(&ffx_api_dispatch));
		if (original == nullptr)
			return -1;

		return original(dispatch_header);
	}

	void try_hook_export(HMODULE module, const char *name, void *replacement, const char *description)
	{
		const auto target = reinterpret_cast<void *>(GetProcAddress(module, name));
		if (target == nullptr || target == replacement)
			return;

		for (const void *hooked : s_hooked_dispatches)
			if (hooked == target)
				return;

		if (reshade::hooks::install(name, target, replacement))
		{
			s_hooked_dispatches.push_back(target);
			reshade::log::message(reshade::log::level::info, "FSR pre-upscale: hooked %s in module %p.", description, module);
		}
	}

	void inspect_module(HMODULE module)
	{
		if (module == nullptr)
			return;

		try_hook_export(module, "ffxFsr3UpscalerContextDispatch",
			reinterpret_cast<void *>(&ffx_fsr3_dispatch), "FSR 3.1 dispatch export");
		try_hook_export(module, "ffxDispatch",
			reinterpret_cast<void *>(&ffx_api_dispatch), "unified FFX dispatch export");
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
	// Present runs every frame. Avoid taking a module snapshot every frame;
	// scan at most once per second to keep the disabled/idle path inexpensive.
	const ULONGLONG now = GetTickCount64();
	ULONGLONG previous = s_last_scan_tick.load(std::memory_order_relaxed);
	if (now - previous < 1000 ||
		!s_last_scan_tick.compare_exchange_strong(previous, now, std::memory_order_relaxed))
		return;

	if (s_scan_in_progress.test_and_set())
		return;

	scan_loaded_modules();
	s_scan_in_progress.clear();
}
