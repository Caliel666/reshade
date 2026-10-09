/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade_api_device.hpp"

namespace reshade
{
	void create_effect_runtime(api::swapchain *swapchain, api::command_queue *graphics_queue, bool vr = false);
	void destroy_effect_runtime(api::swapchain *swapchain);

	void init_effect_runtime(api::swapchain *swapchain);
	void reset_effect_runtime(api::swapchain *swapchain);
	void present_effect_runtime(api::swapchain *swapchain, bool render_effects = true);
	// Returns true once for a frame whose ReShade processing was recorded before FSR.
	bool consume_pre_upscale_frame(api::swapchain *swapchain);

	// Execute the active ReShade runtime on an FSR render-resolution D3D12 color resource.
	// Returns false when no compatible runtime/resource is available; the caller must then
	// forward the original FSR dispatch unchanged.
	bool render_effects_on_external_d3d12_resource(void *command_list, void *resource, uint32_t ffx_resource_state);
}
