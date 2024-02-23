/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef _DUMMY_FE_WRAPPER_H_
#define _DUMMY_FE_WRAPPER_H_

#include <linux/types.h>
#include <linux/ioctl.h>

/* Dummy Frontend Commands */
#define DUMMY_FE_ID 0
#define DUMMY_FE_STATE 1
#define DUMMY_FE_STATUS 2
#define DUMMY_FE_FREQUENCY 3
#define DUMMY_FE_MODULATION 4
#define DUMMY_FE_SYMBOL_RATE 5
#define DUMMY_FE_BANDWIDTH_HZ 6
#define DUMMY_FE_DELIVERY_SYSTEM 7

/* Define dummy fe state machine */
enum dummy_fe_state_t {
	DUMMY_FE_INIT,
	DUMMY_FE_LOCK,
	DUMMY_FE_LOCKED,
	DUMMY_FE_UNLOCKED,
	DUMMY_FE_INVALID
};

/* Define dummy fe demod status */
struct dummy_fe_property {
	__u32 cmd;
	__u32 data;
} __packed;

#define DUMMY_FE_SET_PROPERTY		_IOW('f', 0, struct dummy_fe_property)
#define DUMMY_FE_GET_PROPERTY		_IOR('f', 1, struct dummy_fe_property)
#endif
