/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 *
 * Experimental FSR dispatch interception. This deliberately forwards the
 * original dispatch unchanged until a native-resource effect path is available.
 */
#pragma once

namespace reshade::fsr_pre_upscale
{
	// Called from Present so late-loaded FSR libraries can be discovered.
	void update();
}
