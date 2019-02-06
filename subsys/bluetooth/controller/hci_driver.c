/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <bluetooth/hci_driver.h>
#include <init.h>
#include <irq.h>
#include <kernel.h>
#include <soc.h>

#include <ble_controller.h>
#include <ble_controller_hci.h>
#include "multithreading_lock.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_DEBUG_HCI_DRIVER)
#define LOG_MODULE_NAME bt_ctlr_hci_driver
#include "common/log.h"

static K_SEM_DEFINE(sem_recv, 0, UINT_MAX);
static K_SEM_DEFINE(sem_signal, 0, UINT_MAX);

static struct k_thread recv_thread_data;
static struct k_thread signal_thread_data;
static K_THREAD_STACK_DEFINE(recv_thread_stack, CONFIG_BLECTLR_RX_STACK_SIZE);
static K_THREAD_STACK_DEFINE(signal_thread_stack,
			     CONFIG_BLECTLR_SIGNAL_STACK_SIZE);

static uint8_t ble_controller_mempool[0x6000];

void blectlr_assertion_handler(const char *const file, const u32_t line)
{
#ifdef CONFIG_BT_CTLR_ASSERT_HANDLER
	bt_ctlr_assert_handle(file, line);
#else
	BT_ERR("BleCtlr ASSERT: %s, %d", file, line);
	k_oops();
#endif
}

static int cmd_handle(struct net_buf *cmd)
{
	int32_t errcode = MULTITHREADING_LOCK_ACQUIRE();
	if (!errcode) {
		errcode = hci_cmd_put(cmd->data);
		MULTITHREADING_LOCK_RELEASE();
	}
	if (errcode) {
		return -ENOBUFS;
	}

	k_sem_give(&sem_recv);

	return 0;
}

#if defined(CONFIG_BT_CONN)
static int acl_handle(struct net_buf *acl)
{
	int32_t errcode = MULTITHREADING_LOCK_ACQUIRE();
	if (!errcode) {
		errcode = hci_data_put(acl->data);
		MULTITHREADING_LOCK_RELEASE();
	}
	if (errcode) {
		/* Likely buffer overflow event */
		k_sem_give(&sem_recv);
		return -ENOBUFS;
	}

	return 0;
}
#endif

static int hci_driver_send(struct net_buf *buf)
{
	int32_t  err;
	u8_t type;

	BT_DBG("Enter");

	if (!buf->len) {
		BT_DBG("Empty HCI packet");
		return -EINVAL;
	}

	type = bt_buf_get_type(buf);
	switch (type) {
#if defined(CONFIG_BT_CONN)
	case BT_BUF_ACL_OUT:
		BT_DBG("ACL_OUT");
		err = acl_handle(buf);
		break;
#endif /* CONFIG_BT_CONN */
	case BT_BUF_CMD:
		BT_DBG("CMD");
		err = cmd_handle(buf);
		break;
	default:
		BT_DBG("Unknown HCI type %u", type);
		return -EINVAL;
	}

	if (!err) {
		net_buf_unref(buf);
	}

	BT_DBG("Exit");
	return err;
}

static void data_packet_process(u8_t *hci_buf)
{
	struct net_buf *data_buf = bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER);

	if (!data_buf) {
		BT_ERR("No data buffer available");
		return;
	}

	u16_t handle = hci_buf[0] | (hci_buf[1] & 0xF) << 8;
	u16_t data_length = hci_buf[2] | hci_buf[3] << 8;
	u8_t pb_flag = (hci_buf[1] >> 4) & 0x3;
	u8_t bc_flag = (hci_buf[1] >> 6) & 0x3;

	BT_DBG("Data: Handle(%02x), PB(%01d), "
	       "BC(%01d), Length(%02x)",
	       handle, pb_flag, bc_flag, data_length);

	net_buf_add_mem(data_buf, &hci_buf[0], data_length + 4);
	bt_recv(data_buf);
}

static void event_packet_process(u8_t *hci_buf)
{
	struct bt_hci_evt_hdr *hdr = (void *)hci_buf;
	struct net_buf *evt_buf;

	if (hdr->evt == BT_HCI_EVT_CMD_COMPLETE ||
	    hdr->evt == BT_HCI_EVT_CMD_STATUS) {
		evt_buf = bt_buf_get_cmd_complete(K_FOREVER);
	} else {
		evt_buf = bt_buf_get_rx(BT_BUF_EVT, K_FOREVER);
	}

	if (!evt_buf) {
		BT_ERR("No event buffer available");
		return;
	}

	if (hdr->evt == 0x3E) {
		BT_DBG("LE Meta Event: subevent code "
		       "(%02x), length (%d)",
		       hci_buf[2], hci_buf[1]);
	} else {
		uint8_t opcode = hci_buf[2] << 8 | hci_buf[3];
		BT_DBG("Event: event code (%02x), "
		       "length (%d), "
		       "num_complete (%d), "
		       "opcode (%d)"
		       "status (%d)\n",
		       hci_buf[0], hci_buf[1], hci_buf[2], opcode, hci_buf[5]);
	}

	net_buf_add_mem(evt_buf, &hci_buf[0], hdr->len + 2);
	if (bt_hci_evt_is_prio(hdr->evt)) {
		bt_recv_prio(evt_buf);
	} else {
		bt_recv(evt_buf);
	}
}

static void recv_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static u8_t hci_buffer[256 + 4];
	int32_t errcode;

	BT_DBG("Started");
	while (1) {
		k_sem_take(&sem_recv, K_FOREVER);

		while (1) {
			errcode = MULTITHREADING_LOCK_ACQUIRE();
			if (!errcode) {
				errcode = hci_data_get(hci_buffer);
				MULTITHREADING_LOCK_RELEASE();
			}
			if (!errcode) {
				data_packet_process(hci_buffer);
			} else {
				break;
			}
		};

		while (1) {
			errcode = MULTITHREADING_LOCK_ACQUIRE();
			if (!errcode) {
				errcode = hci_evt_get(hci_buffer);
				MULTITHREADING_LOCK_RELEASE();
			}
			if (!errcode) {
				event_packet_process(hci_buffer);
			} else {
				break;
			}
		};

		/* Let other threads of same priority run in between. */
		k_yield();
	}
}

void _signal_handler_irq(void)
{
	k_sem_give(&sem_recv);
}

static void signal_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&sem_signal, K_FOREVER);
		ble_controller_process_SWI5_IRQ();
	}
}

static int hci_driver_open(void)
{
	BT_DBG("Open");

	k_thread_create(&recv_thread_data, recv_thread_stack,
			K_THREAD_STACK_SIZEOF(recv_thread_stack), recv_thread,
			NULL, NULL, NULL, K_PRIO_COOP(CONFIG_BLECTLR_PRIO), 0,
			K_NO_WAIT);

	k_thread_create(&signal_thread_data, signal_thread_stack,
			K_THREAD_STACK_SIZEOF(signal_thread_stack),
			signal_thread, NULL, NULL, NULL,
			K_PRIO_COOP(CONFIG_BLECTLR_PRIO), 0, K_NO_WAIT);

	return 0;
}

static const struct bt_hci_driver drv = {
	.name = "Controller",
	.bus = BT_HCI_DRIVER_BUS_VIRTUAL,
	.open = hci_driver_open,
	.send = hci_driver_send,
};

void host_signal(void)
{
	/* Wake up the RX event/data thread */
	k_sem_give(&sem_recv);
}

void SIGNALLING_Handler(void)
{
	k_sem_give(&sem_signal);
}

static int ble_init(void)
{
	int32_t err = 0;

	ble_controller_resource_cfg_t resource_cfg;

	resource_cfg.buffer_cfg.rx_packet_size = 251;
	resource_cfg.buffer_cfg.tx_packet_size = 251;
	resource_cfg.conn_event_cfg.event_length_us = 50000;
	resource_cfg.role_cfg.master_count = 1;
	resource_cfg.role_cfg.slave_count = 1;

	err = ble_controller_resource_cfg_set(BLE_CONTROLLER_DEFAULT_RESOURCE_CFG_TAG, &resource_cfg);
	if (err < 0 || err > sizeof(ble_controller_mempool)) {
		return err;
	}

	nrf_lf_clock_cfg_t clock_cfg;

#ifdef CONFIG_CLOCK_CONTROL_NRF5_K32SRC_RC
	clock_cfg.lf_clk_source = NRF_LF_CLOCK_SRC_RC;
#elif CONFIG_CLOCK_CONTROL_NRF5_K32SRC_XTAL
	clock_cfg.lf_clk_source = NRF_LF_CLOCK_SRC_XTAL;
#elif CLOCK_CONTROL_NRF5_K32SRC_SYNTH
	clock_cfg.lf_clk_source = NRF_LF_CLOCK_SRC_SYNTH;
#else
#error "Clock source is not defined"
#endif

#ifdef CONFIG_CLOCK_CONTROL_NRF5_K32SRC_500PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_500_PPM;
#elif CONFIG_CLOCK_CONTROL_NRF5_K32SRC_250PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_250_PPM;
#elif CONFIG_CLOCK_CONTROL_NRF5_K32SRC_150PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_150_PPM;
#elif CONFIG_CLOCK_CONTROL_NRF5_K32SRC_100PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_100_PPM;
#elif CONFIG_CLOCK_CONTROL_NRF5_K32SRC_75PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_75_PPM;
#elif CONFIG_CLOCK_CONTROL_NRF5_K32SRC_50PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_50_PPM;
#elif CONFIG_CLOCK_CONTROL_NRF5_K32SRC_30PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_30_PPM;
#elif CONFIG_CLOCK_CONTROL_NRF5_K32SRC_20PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_20_PPM;
#elif CLOCK_CONTROL_NRF5_K32SRC_10PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_10_PPM;
#elif CLOCK_CONTROL_NRF5_K32SRC_5PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_5_PPM;
#elif CLOCK_CONTROL_NRF5_K32SRC_2PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_2_PPM;
#elif CLOCK_CONTROL_NRF5_K32SRC_1PPM
	clock_cfg.accuracy = NRF_LF_CLOCK_ACCURACY_1_PPM;
#else
#error "Clock accuracy is not defined"
#endif
	clock_cfg.rc_ctiv = BLE_CONTROLLER_RECOMMENDED_RC_CTIV;
	clock_cfg.rc_temp_ctiv = BLE_CONTROLLER_RECOMMENDED_RC_TEMP_CTIV;

	err = MULTITHREADING_LOCK_ACQUIRE();
	if (!err) {
		err = ble_controller_init(blectlr_assertion_handler, &clock_cfg);
		MULTITHREADING_LOCK_RELEASE();
	}
	if (err < 0) {
		return err;
	}

	ble_controller_resource_cfg_t resource_cfg;

	resource_cfg.buffer_cfg.rx_packet_size = 251;
	resource_cfg.buffer_cfg.tx_packet_size = 251;
	resource_cfg.conn_event_cfg.event_length_us = 50000;
	resource_cfg.role_cfg.master_count = 1;
	resource_cfg.role_cfg.slave_count = 1;

	err = MULTITHREADING_LOCK_ACQUIRE();
	if (!err) {
		err = ble_controller_resource_cfg_set(
			BLE_CONTROLLER_DEFAULT_RESOURCE_CFG_TAG, &resource_cfg);
		MULTITHREADING_LOCK_RELEASE();
	}
	if (err < 0 || err > sizeof(ble_controller_mempool)) {
		return err;
	}

	err = MULTITHREADING_LOCK_ACQUIRE();
	if (!err) {
		err =  ble_controller_enable(host_signal, blectlr_assertion_handler,
			&clock_cfg, ble_controller_mempool);
		MULTITHREADING_LOCK_RELEASE();
	}
	if (err < 0) {
		return err;
	}

	return 0;
}

static int hci_driver_init(struct device *unused)
{
	ARG_UNUSED(unused);

	bt_hci_driver_register(&drv);

	int32_t  err = 0;

	err = ble_init();

	if (err < 0) {
		return err;
	}

	IRQ_DIRECT_CONNECT(NRF5_IRQ_RADIO_IRQn, 0,
			   ble_controller_RADIO_IRQHandler, IRQ_ZERO_LATENCY);
	IRQ_DIRECT_CONNECT(NRF5_IRQ_RTC0_IRQn, 0,
			   ble_controller_RTC0_IRQHandler, IRQ_ZERO_LATENCY);
	IRQ_DIRECT_CONNECT(NRF5_IRQ_TIMER0_IRQn, 0,
			   ble_controller_TIMER0_IRQHandler, IRQ_ZERO_LATENCY);
	IRQ_CONNECT(NRF5_IRQ_SWI5_IRQn, 4, SIGNALLING_Handler, NULL, 0);
	IRQ_CONNECT(NRF5_IRQ_RNG_IRQn, 4, ble_controller_RNG_IRQHandler, NULL, 0);
	IRQ_DIRECT_CONNECT(NRF5_IRQ_POWER_CLOCK_IRQn, 0,
			   ble_controller_POWER_CLOCK_IRQHandler,
			   IRQ_ZERO_LATENCY);

	return 0;
}

SYS_INIT(hci_driver_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
