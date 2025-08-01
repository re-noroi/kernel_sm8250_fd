/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2019-2020, The Linux Foundation. All rights reserved. */

#ifndef __CNSS_GENL_H__
#define __CNSS_GENL_H__

enum cnss_genl_msg_type {
	CNSS_GENL_MSG_TYPE_UNSPEC,
	CNSS_GENL_MSG_TYPE_QDSS,
};

int cnss_genl_init(void);
void cnss_genl_exit(void);
int cnss_genl_send_msg(void *buff, u8 type,
		       char *file_name, u32 total_size);

#endif