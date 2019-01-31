/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <soc.h>
#include <errno.h>
#include <device.h>
#include <kernel_includes.h>
#include <clock_control.h>
#include <ble_controller_soc.h>
#include "lock.h"

static inline int ble_controller_hf_clock_request_wlock(
	ble_controller_hf_clock_callback_t on_started)
{
	s32_t errcode;
	API_LOCK_AND_RETURN_ON_FAIL(errcode);
	errcode = ble_controller_hf_clock_request(on_started);
	API_UNLOCK();

	return errcode;
}

static inline int ble_controller_hf_clock_is_running_wlock(bool *p_is_running)
{
	s32_t errcode;
	API_LOCK_AND_RETURN_ON_FAIL(errcode);
	errcode = ble_controller_hf_clock_is_running(p_is_running);
	API_UNLOCK();

	return errcode;
}

static inline int ble_controller_hf_clock_release_wlock(void)
{
	s32_t errcode;
	API_LOCK_AND_RETURN_ON_FAIL(errcode);
	errcode = ble_controller_hf_clock_release();
	API_UNLOCK();

	return errcode;
}

static int hf_clock_start(struct device *dev, clock_control_subsys_t sub_system)
{
	ARG_UNUSED(dev);

	if (ble_controller_hf_clock_request_wlock(NULL) != 0) {
		return -EFAULT;
	}

	bool blocking = POINTER_TO_UINT(sub_system);
	if (blocking)
	{
		bool is_running = false;
		while (!is_running) {
			if (ble_controller_hf_clock_is_running_wlock(&is_running) != 0) {
				return -EFAULT;
			}
		}
	}

	return 0;
}

static int hf_clock_stop(struct device *dev, clock_control_subsys_t sub_system)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sub_system);

	if (ble_controller_hf_clock_release_wlock() != 0) {
		return -EFAULT;
	}

	return 0;
}

static int hf_clock_get_rate(struct device *dev, clock_control_subsys_t sub_system,
							 u32_t *rate)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sub_system);

	if (rate == NULL) {
		return -EINVAL;
	}

	*rate = MHZ(16);
	return 0;
}

static int lf_clock_start(struct device *dev, clock_control_subsys_t sub_system)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sub_system);

	/* No-op. LFCLFK is started by default. */

	return 0;
}

static int lf_clock_get_rate(struct device *dev, clock_control_subsys_t sub_system,
							 u32_t *rate)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sub_system);

	if (rate == NULL) {
		return -EINVAL;
	}

	*rate = 32768;
	return 0;
}

static int clock_control_init(struct device *dev)
{
	ARG_UNUSED(dev);

	/* No-op. Initialized by hci_driver_init() in subsys/bluetooth/controller/hci_driver.c */

	return 0;
}

static const struct clock_control_driver_api hf_clock_control_api = {
	.on = hf_clock_start,
	.off = hf_clock_stop,
	.get_rate = hf_clock_get_rate,
};

DEVICE_AND_API_INIT(hf_clock,
			CONFIG_CLOCK_CONTROL_NRF5_M16SRC_DRV_NAME,
		    clock_control_init, NULL, NULL, PRE_KERNEL_1,
			CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
			&hf_clock_control_api);

/* LFCLK doesn't have stop function to replicate the nRF5 Power Clock driver
 * behavior. */
static const struct clock_control_driver_api lf_clock_control_api = {
	.on = lf_clock_start,
	.off = NULL,
	.get_rate = lf_clock_get_rate,
};

DEVICE_AND_API_INIT(lf_clock,
			CONFIG_CLOCK_CONTROL_NRF5_K32SRC_DRV_NAME,
		    clock_control_init, NULL, NULL, PRE_KERNEL_1,
			CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
			&lf_clock_control_api);

#ifdef UNIT_TEST
struct device *lf_clock_device_get(void)
{
	return &__device_lf_clock;
}

struct device *hf_clock_device_get(void)
{
	return &__device_hf_clock;
}
#endif
