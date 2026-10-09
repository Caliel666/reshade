/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "runtime.hpp"
#include "runtime_manager.hpp"
#include "ini_file.hpp"
#include "d3d12/d3d12_impl_device.hpp"
#include "d3d12/d3d12_impl_command_list.hpp"
#include "addon_manager.hpp"
#include <d3d12.h>
#include <unordered_set>
#include <cassert>
#include <atomic>

static std::shared_mutex s_runtime_config_names_mutex;
static std::unordered_set<std::string> s_runtime_config_names;
static std::shared_mutex s_runtime_instances_mutex;
static std::unordered_set<reshade::runtime *> s_runtime_instances;


namespace
{
	std::atomic<bool> s_pre_upscale_frame_pending = false;

	class pre_upscale_swapchain final : public reshade::api::swapchain
	{
	public:
		pre_upscale_swapchain(reshade::api::swapchain *original, reshade::api::resource back_buffer) :
			_original(original), _back_buffer(back_buffer) {}

		uint64_t get_native() const final { return _original->get_native(); }
		void get_private_data(const uint8_t guid[16], uint64_t *data) const final { _original->get_private_data(guid, data); }
		void set_private_data(const uint8_t guid[16], uint64_t data) final { _original->set_private_data(guid, data); }
		reshade::api::device *get_device() final { return _original->get_device(); }
		void *get_hwnd() const final { return _original->get_hwnd(); }
		reshade::api::resource get_back_buffer(uint32_t index) final { return index == 0 ? _back_buffer : reshade::api::resource{}; }
		uint32_t get_back_buffer_count() const final { return 1; }
		uint32_t get_current_back_buffer_index() const final { return 0; }
		bool check_color_space_support(reshade::api::color_space color_space) const final { return _original->check_color_space_support(color_space); }
		reshade::api::color_space get_color_space() const final { return _original->get_color_space(); }

	private:
		reshade::api::swapchain *_original;
		reshade::api::resource _back_buffer;
	};

	class pre_upscale_command_queue final : public reshade::api::command_queue
	{
	public:
		pre_upscale_command_queue(reshade::api::command_queue *original, reshade::d3d12::device_impl *device, ID3D12GraphicsCommandList *native_list) :
			_original(original), _command_list(device, native_list) {}

		uint64_t get_native() const final { return _original->get_native(); }
		void get_private_data(const uint8_t guid[16], uint64_t *data) const final { _original->get_private_data(guid, data); }
		void set_private_data(const uint8_t guid[16], uint64_t data) final { _original->set_private_data(guid, data); }
		reshade::api::device *get_device() final { return _original->get_device(); }
		reshade::api::command_queue_type get_type() const final { return _original->get_type(); }
		void wait_idle() const final { _original->wait_idle(); }
		// The host owns this command list. Flushing here would submit/close a list
		// that belongs to FSR, so callbacks record into it and let FSR submit it.
		void flush_immediate_command_list() const final {}
		reshade::api::command_list *get_immediate_command_list() final { return &_command_list; }
		void begin_debug_event(const char *label, const float color[4]) final { _command_list.begin_debug_event(label, color); }
		void end_debug_event() final { _command_list.end_debug_event(); }
		void insert_debug_marker(const char *label, const float color[4]) final { _command_list.insert_debug_marker(label, color); }
		bool wait(reshade::api::fence fence, uint64_t value) final { return _original->wait(fence, value); }
		bool signal(reshade::api::fence fence, uint64_t value) final { return _original->signal(fence, value); }
		uint64_t get_timestamp_frequency() const final { return _original->get_timestamp_frequency(); }

	private:
		reshade::api::command_queue *_original;
		reshade::d3d12::command_list_impl _command_list;
	};
}

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
void reshade::present_effect_runtime(api::swapchain *swapchain, bool render_effects)
{
	if (const auto runtime = swapchain->get_private_data<reshade::runtime>())
		runtime->on_present(render_effects);
}

bool reshade::consume_pre_upscale_frame()
{
	return s_pre_upscale_frame_pending.exchange(false, std::memory_order_acq_rel);
}

bool reshade::render_effects_on_external_d3d12_resource(void *command_list, void *resource_ptr, uint32_t ffx_resource_state)
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

				// Translate FidelityFX's abstract resource-state bitmask to D3D12. The
			// dispatch description supplies the state expected by the SDK backend.
			D3D12_RESOURCE_STATES original_state = static_cast<D3D12_RESOURCE_STATES>(0);
			if (ffx_resource_state & (1u << 2)) original_state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			if (ffx_resource_state & (1u << 3)) original_state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			if (ffx_resource_state & (1u << 4)) original_state |= D3D12_RESOURCE_STATE_COPY_SOURCE;
			if (ffx_resource_state & (1u << 5)) original_state |= D3D12_RESOURCE_STATE_COPY_DEST;
			if (ffx_resource_state & (1u << 1)) original_state |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			if (ffx_resource_state & (1u << 6)) original_state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
			if (ffx_resource_state & (1u << 7)) original_state |= D3D12_RESOURCE_STATE_PRESENT;
			if (ffx_resource_state & (1u << 8)) original_state |= D3D12_RESOURCE_STATE_RENDER_TARGET;
			if (original_state == 0 && (ffx_resource_state & 1u)) original_state = D3D12_RESOURCE_STATE_COMMON;
			if (original_state == 0)
			{
				device->destroy_resource_view(rtv);
				continue;
			}

			// Record effects on the same command list so their writes are ordered before FSR.
			D3D12_RESOURCE_BARRIER to_rtv = {};
			to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			to_rtv.Transition.pResource = native_resource;
			to_rtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			to_rtv.Transition.StateBefore = original_state;
			to_rtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			const bool needs_transition = original_state != D3D12_RESOURCE_STATE_RENDER_TARGET;
			if (needs_transition)
				native_list->ResourceBarrier(1, &to_rtv);

			reshade::d3d12::command_list_impl reshade_list(device, native_list);
			runtime->render_effects(&reshade_list, rtv, {});

			// Route the legacy ReShade add-on 'present' event through the same native
			// command list, but expose the FSR input as the swap chain's sole back buffer.
#if RESHADE_ADDON
			if (reshade::has_addon_event<reshade::addon_event::present>())
			{
				D3D12_RESOURCE_BARRIER to_present = {};
				to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				to_present.Transition.pResource = native_resource;
				to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				to_present.Transition.StateBefore = original_state;
				to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
				if (original_state != D3D12_RESOURCE_STATE_PRESENT)
					native_list->ResourceBarrier(1, &to_present);

				pre_upscale_swapchain external_swapchain(runtime->get_swapchain(), api_resource);
				pre_upscale_command_queue external_queue(runtime->get_command_queue(), device, native_list);
				reshade::invoke_addon_event<reshade::addon_event::present>(
					&external_queue, &external_swapchain, nullptr, nullptr, 0, nullptr);

				if (original_state != D3D12_RESOURCE_STATE_PRESENT)
				{
					D3D12_RESOURCE_BARRIER back_to_original = to_present;
					back_to_original.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
					back_to_original.Transition.StateAfter = original_state;
					native_list->ResourceBarrier(1, &back_to_original);
				}
			}
#endif
			if (needs_transition)
			{
				D3D12_RESOURCE_BARRIER back_to_original = to_rtv;
				back_to_original.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
				back_to_original.Transition.StateAfter = original_state;
				native_list->ResourceBarrier(1, &back_to_original);
			}

			device->destroy_resource_view(rtv);
			rendered = true;
			s_pre_upscale_frame_pending.store(true, std::memory_order_release);
			break;
		}
	}
	native_device->Release();
	return rendered;
}
