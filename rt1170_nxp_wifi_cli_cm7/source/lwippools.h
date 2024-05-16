/* @file lwippools.h
 *
 *  @brief This file contains custom LwIP memory pool definitions
 *
 *  Copyright 2020, 2023 NXP
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __LWIPPOOLS_H__
#define __LWIPPOOLS_H__

#ifdef MEMP_USE_CUSTOM_POOLS
/*
 * We explicitly move certain large LwIP memory pools to the custom defined
 * .wlan_data section in (flash) memory to avoid memory overflow in the
 * m_data section (RAM).
 */
extern unsigned char __attribute__((section(".gsd.$BOARD_SDRAM"))) memp_memory_PBUF_POOL_base[];
extern unsigned char __attribute__((section(".gsd.$BOARD_SDRAM"))) memp_memory_TCP_PCB_POOL_base[];
extern unsigned char __attribute__((section(".gsd.$BOARD_SDRAM"))) memp_memory_TCP_SEG_base[];
extern unsigned char __attribute__((section(".gsd.$BOARD_SDRAM"))) memp_memory_REASSDATA_base[];
extern unsigned char __attribute__((section(".gsd.$BOARD_SDRAM"))) memp_memory_TCPIP_MSG_API_base[];
extern unsigned char __attribute__((section(".gsd.$BOARD_SDRAM"))) memp_memory_TCPIP_MSG_INPKT_base[];
extern unsigned char __attribute__((section(".gsd.$BOARD_SDRAM"))) memp_memory_PBUF_base[];
extern unsigned char __attribute__((section(".gsd.$BOARD_SDRAM"))) ram_heap[];

#endif /* MEMP_USE_CUSTOM_POOLS */

#endif /* __LWIPPOOLS_H__ */