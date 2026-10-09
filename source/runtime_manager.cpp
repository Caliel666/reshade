/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "runtime.hpp"
#include "runtime_manager.hpp"
#include "ini_file.hpp"
#include "d3d12/d3d12_impl_device.hpp"
#include "d3d12/d3d12_impl_command_list.hpp"
#include <d3d12.h>
#include <unordered_set>
#include <cassert>
#include <unordered_set>

static std::shared_mutex s_runtime_config_names_mutex;
static std::unordered_set<std::string> s_runtime_config_names;
static std::shared_mutex s_runtime_instances_mutex;
static std::unordered_set<reshade::runtime *> s_runtime_instances;

void reshade::create_effect_runtime(api::swapchain *swapchain, api::command_queue *graphics_queue, bool vr)
{
	if (graphics_queue == nullptr || swapchain->get_private_data<reshade::runtime>() != nullptr)
		return;

	assert((graphics_queue->get_type() & api::command_queue_type::graphics) != 0);

	// Try to find a unique configuration name for this effect runtime instance
	std::string config_name = "ReShade";
	if (vr)
		config_name += "VR";
	{
		const std::string config_name_base = config_name;

		const std::unique_lock<std::shared_mutex> lock(s_runtime_config_names_mutex);

		if (size_t max_runtimes = std::numeric_limits<size_t>::max();
			global_config().get("INSTALL", "MaxEffectRuntimes", max_runtimes) &&
			s_runtime_config_names.size() >= max_runtimes)
			return;

		for (int attempt = 1; attempt < 100 && s_runtime_config_names.find(config_name) != s_runtime_config_names.end(); ++attempt)
			config_name = config_name_base + std::to_string(attempt + 1);

		assert(s_runtime_config_names.find(config_name) == s_runtime_config_names.end());
		s_runtime_config_names.insert(config_name);
	}

	const ini_file &config = ini_file::load_cache(g_reshade_base_path / std::filesystem::u8path(config_name + ".ini"));
	if (config.get("GENERAL", "Disable"))
		return;

	swapchain->create_private_data<reshade::runtime>(swapchain, graphics_queue, config.path(), vr);
	if (auto *runtime = swapchain->get_private_data<reshade::runtime>())
	{
		const std::unique_lock<std::shared_mutex> lock(s_runtime_instances_mutex);
		s_runtime_instances.insert(runtime);
	}
}
void reshade::destroy_effect_runtime(api::swapchain *swapchain)
{
	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
	{
		const std::string config_name = runtime->get_config_path().stem().u8string();

		// Free up the configuration name of this effect runtime instance for reuse
		const std::unique_lock<std::shared_mutex> lock(s_runtime_config_names_mutex);

		s_runtime_config_names.erase(config_name);
	}

	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
	{
		const std::unique_lock<std::shared_mutex> lock(s_runtime_instances_mutex);
		s_runtime_instances.erase(runtime);
	}
	swapchain->destroy_private_data<reshade::runtime>();
}

void reshade::init_effect_runtime(api::swapchain *swapchain)
{
	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
		runtime->on_init();
}
void reshade::reset_effect_runtime(api::swapchain *swapchain)
{
	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
		runtime->on_reset();
}
void reshade::present_effect_runtime(api::swapchain *swapchain)
{
	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
		runtime->on_present();
}

bool reshade::render_effects_on_external_d3d12_resource(void *command_list, void *resource_ptr)
{
	if (command_list == nullptr || resource_ptr == nullptr)
		return false;

	auto *native_list = static_cast<ID3D12GraphicsCommandList *>(command_list);
	auto *native_resource = static_cast<ID3D12Resource *>(resource_ptr);
	const D3D12_RESOURCE_DESC native_desc = native_resource->GetDesc();

	// ReShade's effect pipeline renders into an RTV. Do not try to fabricate one for
	// textures that D3D12 explicitly forbids using as render targets.
	if (native_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
		native_desc.SampleDesc.Count != 1 ||
		(native_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
		return false;

	ID3D12Device *native_device = nullptr;
	if (FAILED(native_resource->GetDevice(IID_PPV_ARGS(&native_device))))
		return false;

	bool rendered = false;
	{
		const std::shared_lock<std::shared_mutex> lock(s_runtime_instances_mutex);
		for (reshade::runtime *runtime : s_runtime_instances)
		{
			if (runtime == nullptr || runtime->get_device() == nullptr ||
				runtime->get_device()->get_api() != reshade::api::device_api::d3d12 ||
				runtime->get_device()->get_native() != reinterpret_cast<uint64_t>(native_device))
				continue;

			auto *device = static_cast<reshade::d3d12::device_impl *>(runtime->get_device());
			const reshade::api::resource api_resource { reinterpret_cast<uint64_t>(native_resource) };
			reshade::api::resource_view rtv = {};
			reshade::api::resource_view_desc view_desc = {};
			if (!device->create_resource_view(api_resource, reshade::api::resource_usage::render_target, view_desc, &rtv))
				continue;

			// FSR's color input is normally in a shader-readable state before dispatch.
			// Record effects on the same command list so their writes are ordered before FSR.
			D3D12_RESOURCE_BARRIER to_rtv = {};
			to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			to_rtv.Transition.pResource = native_resource;
			to_rtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			to_rtv.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			to_rtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			native_list->ResourceBarrier(1, &to_rtv);

			reshade::d3d12::command_list_impl reshade_list(device, native_list);
			runtime->render_effects(&reshade_list, rtv, 0);

			D3D12_RESOURCE_BARRIER back_to_srv = to_rtv;
			back_to_srv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			back_to_srv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			native_list->ResourceBarrier(1, &back_to_srv);

			device->destroy_resource_view(rtv);
			rendered = true;
			break;
		}
	}
	native_device->Release();
	return rendered;
}
