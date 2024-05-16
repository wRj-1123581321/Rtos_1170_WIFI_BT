/** @file wifi-sdio.c
 *
 *  @brief  This file provides WLAN Card related API
 *
 *  Copyright 2008-2022 NXP
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <mlan_api.h>

#include <mlan_sdio_api.h>


/* Additional WMSDK header files */
#include <wmerrno.h>
#include <wm_os.h>
#include <wm_utils.h>
#include <mlan_fw.h>
#include <mlan_wmm.h>
#include <netif_decl.h>
#include "wifi-sdio.h"
#include "wifi-internal.h"
#include "fsl_sdmmc_common.h"
#include "fsl_sdmmc_host.h"
#include "fsl_common.h"
#include "sdmmc_config.h"
#include "sdio.h"
#include "firmware_dnld.h"
#include "fwdnld_sdio.h"

#define SDIO_COMMAND_RESPONSE_WAIT_MS 20000

/* Buffer pointers to point to command and, command response buffer */
static uint8_t ctrl_cmd_buf[WIFI_FW_CMDBUF_SIZE];
#ifdef CONFIG_FW_VDLL
static uint8_t vdll_cmd_buf[WIFI_FW_CMDBUF_SIZE] = {0};
#endif
static int seqnum;
// static int pm_handle;

/*
 * Used to authorize the SDIO interrupt handler to accept the incoming
 * packet from the SDIO interface. If this flag is set a semaphore is
 * signalled.
 */
bool g_txrx_flag;

int mlan_subsys_init(void);
int mlan_subsys_deinit(void);
void set_ioport_inmlan(t_u32 port);

static t_u8 txportno;

static t_u32 last_cmd_sent, fw_init_cfg;

static os_mutex_t txrx_mutex;
#ifdef CONFIG_WIFI_IND_RESET
static os_mutex_t ind_reset_mutex;
#endif
static os_semaphore_t sdio_command_resp_sem;
os_thread_t wifi_core_thread;

static struct
{
    /* Where the cmdresp/event should be dispached depends on its value */
    /* int special; */
    /* Default queue where the cmdresp/events will be sent */
    os_queue_t *event_queue;
    int (*wifi_low_level_input)(const uint8_t interface, const uint8_t *buffer, const uint16_t len);
} bus;

/* fixme: This structure is not present in mlan and can be removed later */
typedef MLAN_PACK_START struct
{
    t_u16 size;
    t_u16 pkttype;
    HostCmd_DS_COMMAND hostcmd;
} MLAN_PACK_END SDIOPkt;

static SDIOPkt *sdiopkt = (SDIOPkt *)outbuf;

void wrapper_wlan_cmd_11n_cfg(HostCmd_DS_COMMAND *cmd);

static uint32_t dev_value1 = -1;
uint8_t dev_mac_addr[MLAN_MAC_ADDR_LENGTH];
uint8_t dev_mac_addr_uap[MLAN_MAC_ADDR_LENGTH];
static uint8_t dev_fw_ver_ext[MLAN_MAX_VER_STR_LEN];

int wifi_sdio_lock(void)
{
    return os_mutex_get(&txrx_mutex, OS_WAIT_FOREVER);
}

void wifi_sdio_unlock(void)
{
    (void)os_mutex_put(&txrx_mutex);
}

#ifdef CONFIG_WIFI_IND_RESET
int wifi_ind_reset_lock(void)
{
    return os_mutex_get(&ind_reset_mutex, OS_WAIT_FOREVER);
}

void wifi_ind_reset_unlock(void)
{
    (void)os_mutex_put(&ind_reset_mutex);
}
#endif

static int wifi_sdio_get_command_resp_sem(unsigned long wait)
{
    return os_semaphore_get(&sdio_command_resp_sem, wait);
}

static int wifi_sdio_put_command_resp_sem(void)
{
    return os_semaphore_put(&sdio_command_resp_sem);
}

static void wifi_sdio_wait_for_cmdresp()
{
    /* Wait max 20 sec for the command response */
    int ret = wifi_sdio_get_command_resp_sem(os_msec_to_ticks(SDIO_COMMAND_RESPONSE_WAIT_MS));
    if (ret != WM_SUCCESS)
    {
        /* assert as command flow cannot work anymore */
        assert(0);
    }
}

uint32_t wifi_get_device_value1(void)
{
    return dev_value1;
}

int wifi_get_device_mac_addr(wifi_mac_addr_t *mac_addr)
{
    (void)memcpy(mac_addr->mac, dev_mac_addr, MLAN_MAC_ADDR_LENGTH);
    return WM_SUCCESS;
}

int wifi_get_device_uap_mac_addr(wifi_mac_addr_t *mac_addr_uap)
{
    (void)memcpy(mac_addr_uap->mac, dev_mac_addr_uap, MLAN_MAC_ADDR_LENGTH);
    return WM_SUCCESS;
}

int wifi_get_device_firmware_version_ext(wifi_fw_version_ext_t *fw_ver_ext)
{
    (void)memcpy((void *)fw_ver_ext->version_str, (const void *)dev_fw_ver_ext, MLAN_MAX_VER_STR_LEN);
    return WM_SUCCESS;
}

/* Initializes the driver struct */
static int wlan_init_struct(void)
{
    if (txrx_mutex == MNULL)
    {
        int status = os_mutex_create(&txrx_mutex, "txrx", OS_MUTEX_INHERIT);
        if (status != WM_SUCCESS)
        {
            return status;
        }
    }
#ifdef CONFIG_WIFI_IND_RESET
    if (ind_reset_mutex == MNULL)
    {
        int status = os_mutex_create(&ind_reset_mutex, "ind_reset", OS_MUTEX_INHERIT);
        if (status != WM_SUCCESS)
        {
            return status;
        }
    }
#endif
    if (sdio_command_resp_sem == MNULL)
    {
        int status = os_semaphore_create(&sdio_command_resp_sem, "sdio command resp sem");

        if (status != WM_SUCCESS)
        {
            return status;
        }
    }

    /*
     * Take the sdio cmd resp lock immediately so that we can later block on
     * it.
     */
    (void)wifi_sdio_get_command_resp_sem(OS_WAIT_FOREVER);

    return WM_SUCCESS;
}

static int wlan_deinit_struct(void)
{
    if (txrx_mutex != MNULL)
    {
        int status = os_mutex_delete(&txrx_mutex);
        if (status != WM_SUCCESS)
        {
            wifi_io_e("%s mutex deletion error %d", __FUNCTION__, status);
            return status;
        }
        txrx_mutex = MNULL;
    }
    else
    {
        wifi_io_d("%s mutex does not exsit", __FUNCTION__);
    }

#ifdef CONFIG_WIFI_IND_RESET
    if (ind_reset_mutex != MNULL)
    {
        int status = os_mutex_delete(&ind_reset_mutex);
        if (status != WM_SUCCESS)
        {
            wifi_io_e("%s mutex deletion error %d", __FUNCTION__, status);
            return status;
        }
        ind_reset_mutex = MNULL;
    }
    else
    {
        wifi_io_d("%s mutex does not exsit", __FUNCTION__);
    }
#endif
    if (sdio_command_resp_sem != MNULL)
    {
        int status = os_semaphore_delete(&sdio_command_resp_sem);
        if (status != WM_SUCCESS)
        {
            wifi_io_e("%s mutex deletion error %d", __FUNCTION__, status);
            return status;
        }
        sdio_command_resp_sem = MNULL;
    }
    else
    {
        wifi_io_d("%s semaphore does not exsit", __FUNCTION__);
    }

    (void)memset(dev_mac_addr, 0, sizeof(dev_mac_addr));
    (void)memset(dev_fw_ver_ext, 0, sizeof(dev_fw_ver_ext));

    return WM_SUCCESS;
}

int raw_process_pkt_hdrs(void *pbuf, t_u32 payloadlen, t_u8 interface)
{
    mlan_private *pmpriv = (mlan_private *)mlan_adap->priv[interface];
    SDIOPkt *sdiohdr     = (SDIOPkt *)pbuf;
    TxPD *ptxpd          = (TxPD *)(void *)((uint8_t *)pbuf + INTF_HEADER_LEN);

    ptxpd->bss_type      = interface;
    ptxpd->bss_num       = GET_BSS_NUM(pmpriv);
    ptxpd->tx_pkt_offset = 0x14; /* we'll just make this constant */
    ptxpd->tx_pkt_length = (t_u16)(payloadlen - ptxpd->tx_pkt_offset - INTF_HEADER_LEN);
    ptxpd->tx_pkt_type   = 0xE5;
    ptxpd->tx_control    = 0;
    ptxpd->priority      = 0;
    ptxpd->flags         = 0;
    ptxpd->pkt_delay_2ms = 0;

#ifdef CONFIG_WPA_SUPP
    if (pmpriv->tx_seq_num == 0)
    {
        pmpriv->tx_seq_num++;
    }
    ptxpd->tx_token_id = pmpriv->tx_seq_num++;
    ptxpd->flags |= MRVDRV_TxPD_FLAGS_TX_PACKET_STATUS;
#endif

    sdiohdr->size = (t_u16)(payloadlen + ptxpd->tx_pkt_offset + INTF_HEADER_LEN);

    return (int)(ptxpd->tx_pkt_offset + INTF_HEADER_LEN);
}

/*
 * fixme: mlan_sta_tx.c can be used directly here. This functionality is
 * already present there.
 */
/* SDIO  TxPD  PAYLOAD | 4 | 22 | payload | */

void process_pkt_hdrs(void *pbuf, t_u32 payloadlen, t_u8 interface, t_u8 tid, t_u32 tx_control)
{
    mlan_private *pmpriv = (mlan_private *)mlan_adap->priv[interface];
#ifdef CONFIG_FW_VDLL
    mlan_adapter *pmadapter = pmpriv->adapter;
#endif
    SDIOPkt *sdiohdr     = (SDIOPkt *)pbuf;
    TxPD *ptxpd          = (TxPD *)(void *)((uint8_t *)pbuf + INTF_HEADER_LEN);

    ptxpd->bss_type      = interface;
    ptxpd->bss_num       = GET_BSS_NUM(pmpriv);
    ptxpd->tx_pkt_offset = 0x16; /* we'll just make this constant */						// 跳过 sizeof(TxPD)
    ptxpd->tx_pkt_length = (t_u16)(payloadlen - ptxpd->tx_pkt_offset - INTF_HEADER_LEN);	// 实际data的大小 p->tot_len
    ptxpd->tx_control    = tx_control;
    ptxpd->priority      = tid;
    ptxpd->flags         = 0;

    if (ptxpd->tx_pkt_type == 0xe5U)
    {
        ptxpd->tx_pkt_offset = 0x14; /* Override for special frame */
    }

    ptxpd->pkt_delay_2ms = 0;

    sdiohdr->size = (t_u16)payloadlen;			// SDIOPkt + sizeof(TxPD) + p->tot_len

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif
}

void process_pkt_hdrs_flags(void *pbuf, t_u8 flags)
{
    TxPD *ptxpd  = (TxPD *)((uint8_t *)pbuf + INTF_HEADER_LEN);
    ptxpd->flags = flags;
}

int bus_register_event_queue(os_queue_t *event_queue)
{
    if (bus.event_queue != NULL)
    {
        return -WM_FAIL;
    }

    bus.event_queue = event_queue;

    return WM_SUCCESS;
}

void bus_deregister_event_queue(void)
{
    if (bus.event_queue != NULL)
    {
        bus.event_queue = NULL;
    }
}

int bus_register_data_input_function(int (*wifi_low_level_input)(const uint8_t interface,
                                                                 const uint8_t *buffer,
                                                                 const uint16_t len))
{
    if (bus.wifi_low_level_input != NULL)
    {
        return -WM_FAIL;
    }

    bus.wifi_low_level_input = wifi_low_level_input;

    return WM_SUCCESS;
}

void bus_deregister_data_input_funtion(void)
{
    bus.wifi_low_level_input = NULL;
}


void wifi_get_mac_address_from_cmdresp(const HostCmd_DS_COMMAND *resp, uint8_t *mac_addr);
void wifi_get_firmware_ver_ext_from_cmdresp(const HostCmd_DS_COMMAND *resp, uint8_t *fw_ver_ext);
void wifi_get_value1_from_cmdresp(const HostCmd_DS_COMMAND *resp, uint32_t *dev_value1);

#ifdef CONFIG_FW_VDLL
static mlan_status wlan_handle_event_packet(t_u8 *pmbuf)
{
    t_u16 evttype      = wlan_le16_to_cpu(*(t_u16 *)pmbuf);
    mlan_status status = MLAN_STATUS_SUCCESS;

#ifdef CONFIG_FW_VDLL_DEBUG
    wevt_d("Event %x", evttype);
#endif

    switch (evttype)
    {
        case EVENT_VDLL_IND:
        {
            status = wlan_process_vdll_event((mlan_private *)mlan_adap->priv[0], pmbuf);
            break;
        }
        default:
            wevt_d("Unhandled pre Queue event %x", evttype);
            break;
    }

    return status;
}
#endif

static mlan_status wlan_handle_cmd_resp_packet(t_u8 *pmbuf)
{
    HostCmd_DS_GEN *cmdresp;
    t_u32 cmdtype;
    int bss_type;

    cmdresp = (HostCmd_DS_GEN *)(void *)(pmbuf + INTF_HEADER_LEN); /* size + pkttype=4 */
    cmdtype = cmdresp->command & HostCmd_CMD_ID_MASK;

    bss_type = HostCmd_GET_BSS_TYPE(cmdresp->seq_num);

    if ((cmdresp->command & 0xf000) != 0x8000)
    {
        wifi_io_d("cmdresp->command = (0x%x)", cmdresp->command);
    }

    /* Do not process response of wlan firmware shutdown command
     *
     * This is required to flush out any previous response
     * from the wlan_deinit() which might have been called
     * prior to this.
     *
     */
    if ((cmdresp->command & 0x00ff) == HostCmd_CMD_FUNC_SHUTDOWN)
    {
        return MLAN_STATUS_SUCCESS;
    }

    if ((cmdresp->command & 0x0fffU) != last_cmd_sent)
    {
        wifi_io_d("cmdresp->command = (0x%x) last_cmd_sent = (0x%x)", cmdresp->command, last_cmd_sent);
    }

    if (cmdresp->result != 0U)
    {
        wifi_io_d("cmdresp->result = (0x%x)", cmdresp->result);
    }

    wifi_io_d("Resp : (0x%x)", cmdtype);
    switch (cmdtype)
    {
        case HostCmd_CMD_FUNC_INIT:
        case HostCmd_CMD_MAC_CONTROL:
        case HostCmd_CMD_CFG_DATA:
            break;
        case HostCmd_CMD_MAC_REG_ACCESS:
            wifi_get_value1_from_cmdresp((HostCmd_DS_COMMAND *)(void *)cmdresp, &dev_value1);
            break;
        case HostCmd_CMD_802_11_MAC_ADDRESS:
            if (bss_type == MLAN_BSS_TYPE_UAP)
            {
                wifi_get_mac_address_from_cmdresp((HostCmd_DS_COMMAND *)(void *)cmdresp, dev_mac_addr_uap);
            }
            else
            {
                wifi_get_mac_address_from_cmdresp((HostCmd_DS_COMMAND *)(void *)cmdresp, dev_mac_addr);
            }
            break;
#ifdef OTP_CHANINFO
        case HostCmd_CMD_CHAN_REGION_CFG:
            (void)wlan_ret_chan_region_cfg((mlan_private *)mlan_adap->priv[0], (HostCmd_DS_COMMAND *)(void *)cmdresp,
                                           NULL);
            break;
#endif
        case HostCmd_CMD_GET_HW_SPEC:
            (void)wlan_ret_get_hw_spec((mlan_private *)mlan_adap->priv[0], (HostCmd_DS_COMMAND *)(void *)cmdresp, NULL);
            break;
        case HostCmd_CMD_VERSION_EXT:
            wifi_get_firmware_ver_ext_from_cmdresp((HostCmd_DS_COMMAND *)(void *)cmdresp, dev_fw_ver_ext);
            break;
        case HostCmd_CMD_11N_CFG:
        case HostCmd_CMD_AMSDU_AGGR_CTRL:
            break;
        case HostCmd_CMD_FUNC_SHUTDOWN:
            break;
#ifdef WLAN_LOW_POWER_ENABLE
        case HostCmd_CMD_LOW_POWER_MODE:
            break;
#endif
        case HostCmd_CMD_ED_MAC_MODE:
        case HostCmd_CMD_CHANNEL_TRPC_CONFIG:
            break;
        case HostCmd_CMD_RECONFIGURE_TX_BUFF:
            break;
        default:
            wifi_io_d("Unimplemented Resp : (0x%x)", cmdtype);
#ifdef CONFIG_WIFI_IO_DUMP
            dump_hex(cmdresp, cmdresp->size);
#endif
            break;
    }

    wifi_sdio_put_command_resp_sem();

    return MLAN_STATUS_SUCCESS;
}

/*
 * Accepts event and command packets. Redirects them to queues if
 * registered. If queues are not registered (as is the case during
 * initialization then the packet is given to lower layer cmd/event
 * handling part.
 */
static mlan_status wlan_decode_rx_packet(t_u8 *pmbuf, t_u32 upld_type)
{
    if (upld_type == MLAN_TYPE_DATA)
    {
        return MLAN_STATUS_FAILURE;
    }

    if (upld_type == MLAN_TYPE_CMD)
    {
        wifi_io_d("  --- Rx: Cmd Response ---");
    }
    else
    {
        wifi_io_d(" --- Rx: EVENT Response ---");
    }

    SDIOPkt *sdiopkt = (SDIOPkt *)(void *)pmbuf;

#ifdef CONFIG_FW_VDLL
    if (upld_type == MLAN_TYPE_EVENT && (wlan_le16_to_cpu(sdiopkt->hostcmd.command) == EVENT_VDLL_IND))
    {
#ifdef CONFIG_FW_VDLL_DEBUG
        dump_hex(pmbuf, sdiopkt->size);
#endif
        return wlan_handle_event_packet(pmbuf + INTF_HEADER_LEN);
    }
#endif
#ifdef CONFIG_CSI
    if (upld_type == MLAN_TYPE_EVENT && (wlan_le16_to_cpu(sdiopkt->hostcmd.command) == EVENT_CSI))
    {
        csi_save_data_to_local_buff((t_u8 *)pmbuf + 8);
    }
#endif

    int ret;
    struct bus_message msg;

        if ((fw_init_cfg == 0U) && (bus.event_queue != NULL))
    {
        if (upld_type == MLAN_TYPE_CMD)
        {
            msg.data = wifi_mem_malloc_cmdrespbuf();
        }
        else
        {
            msg.data = wifi_malloc_eventbuf((size_t)sdiopkt->size);
        }

        if (msg.data == MNULL)
        {
            wifi_io_e("[fail] Buffer alloc: T: %d S: %d", upld_type, sdiopkt->size);
            return MLAN_STATUS_FAILURE;
        }

        msg.event = (uint16_t)upld_type;
        (void)memcpy((void *)msg.data, (const void *)pmbuf, sdiopkt->size);

#ifdef CONFIG_WMM
        if (upld_type == MLAN_TYPE_EVENT && sdiopkt->hostcmd.command == EVENT_TX_DATA_PAUSE)
        {
            wifi_handle_event_data_pause(msg.data);
            wifi_free_eventbuf(msg.data);
            return MLAN_STATUS_SUCCESS;
        }
#endif
        ret = os_queue_send(bus.event_queue, &msg, os_msec_to_ticks(WIFI_RESP_WAIT_TIME));

        if (ret != WM_SUCCESS)
        {
            wifi_io_e("Failed to send response on Queue");
            return MLAN_STATUS_FAILURE;
        }
    }
    else
    {
        /* No queues registered yet. Use local handling */
        if (upld_type == MLAN_TYPE_CMD)
        {
            (void)wlan_handle_cmd_resp_packet(pmbuf);
        }
    }

    return MLAN_STATUS_SUCCESS;
}

#if 0
static t_u32 get_ioport(void)
{
    return mlan_adap->ioport;
}
#endif

static t_u8 *wlan_read_rcv_packet(t_u32 port, t_u32 rxlen, t_u32 rx_blocks, t_u32 *type, bool aggr)
{
    t_u32 blksize = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;
    int ret;
#ifdef CONFIG_RX_POOL
	inbuf_t *pbuf = NULL;
	pbuf = wifi_wmm_inbuf_get();
#endif

#ifdef CONFIG_SDIO_MULTI_PORT_RX_AGGR
    int i = 0;

    while (true)
    {
        /* addr = 0 fn = 1 */
#ifdef CONFIG_RX_POOL
        ret = sdio_drv_read(port, 1, rx_blocks, blksize, pbuf->data, &resp);
#else
		ret = sdio_drv_read(port, 1, rx_blocks, blksize, inbuf, &resp);
#endif

        if (aggr && !ret)
        {
            wifi_d("sdio mp cmd53 read failed: %d ioport=0x%x retry=%d\r\n", ret, port, i);
            i++;
            if (sdio_drv_creg_write(HOST_TO_CARD_EVENT_REG, 1, HOST_TERM_CMD53, &resp) == false)
            {
                wifi_d("Set Term cmd53 failed\r\n");
            }
            if (i > MAX_READ_IOMEM_RETRY)
            {
                wifi_io_e("sdio_drv_read failed (%d)", ret);
#ifdef CONFIG_WIFI_FW_DEBUG
                wifi_sdio_reg_dbg(NULL);
                if (wm_wifi.wifi_usb_mount_cb != NULL)
                {
                    ret = wm_wifi.wifi_usb_mount_cb();
                    if (ret == WM_SUCCESS)
                    {
                        wifi_dump_firmware_info(NULL);
                    }
                    else
                    {
                        wifi_e("USB mounting failed");
                    }
                }
                else
                {
                    wifi_e("USB mount callback is not registered");
                }
#endif
                return NULL;
            } /* if (i > MAX_READ_IOMEM_RETRY) */
            continue;
        } /* if (aggr && !ret) */
        break;
    } /* while(true) */
#else
    /* addr = 0 fn = 1 */
#ifdef CONFIG_RX_POOL
    ret = sdio_drv_read(port, 1, rx_blocks, blksize, pbuf->data, &resp);
#else
	ret = sdio_drv_read(port, 1, rx_blocks, blksize, inbuf, &resp);
#endif
    if (!ret)
    {
        wifi_io_e("sdio_drv_read failed (%d)", ret);
#ifdef CONFIG_WIFI_FW_DEBUG
        wifi_sdio_reg_dbg(NULL);
        if (wm_wifi.wifi_usb_mount_cb != NULL)
        {
            ret = wm_wifi.wifi_usb_mount_cb();
            if (ret == WM_SUCCESS)
            {
                wifi_dump_firmware_info(NULL);
            }
            else
            {
                wifi_e("USB mounting failed");
            }
        }
        else
        {
            wifi_e("USB mount callback is not registered");
        }

#endif
        return NULL;
    }
#endif

#ifdef CONFIG_RX_POOL
    SDIOPkt *insdiopkt = (SDIOPkt *)(void *)pbuf->data;
#else
	SDIOPkt *insdiopkt = (SDIOPkt *)(void *)inbuf;
#endif
    *type              = insdiopkt->pkttype;

#ifdef CONFIG_WIFI_IO_DUMP
    if (insdiopkt->pkttype != 0)
    {
        (void)PRINTF("wlan_read_rcv_packet: DUMP:");
#ifdef CONFIG_RX_POOL
        dump_hex((t_u8 *)pbuf->data, rx_blocks * blksize);
#else
		dump_hex((t_u8 *)inbuf, rx_blocks * blksize);
#endif
    }
#endif /* CONFIG_WIFI_IO_DUMP */

#ifdef CONFIG_RX_POOL
    return (t_u8*)pbuf;
#else
	return inbuf;
#endif
}

static int wlan_get_next_seq_num(void)
{
    seqnum++;
    return seqnum;
}

void wifi_prepare_set_cal_data_cmd(HostCmd_DS_COMMAND *cmd, int seq_number);
static void _wlan_set_cal_data(void)
{
    t_u32 tx_blocks = 4, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, SDIO_OUTBUF_LEN);

    /* sdiopkt = outbuf */
    wifi_prepare_set_cal_data_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_CFG_DATA;

    /* send CMD53 to write the command to get mac address */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();
}

void wifi_prepare_reconfigure_tx_buf_cmd(HostCmd_DS_COMMAND *cmd, t_u16 seq_number);

static void wlan_reconfigure_tx_buffers(void)
{
    t_u32 tx_blocks = 4, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, SDIO_OUTBUF_LEN);

    /* sdiopkt = outbuf */
    wifi_prepare_reconfigure_tx_buf_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());
    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_RECONFIGURE_TX_BUFF;

    /* send CMD53 to write the command to get mac address */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();
}

void wifi_prepare_get_mac_addr_cmd(HostCmd_DS_COMMAND *cmd, int seq_number);
#ifdef OTP_CHANINFO
void wifi_prepare_get_channel_region_cfg_cmd(HostCmd_DS_COMMAND *cmd, t_u16 seq_number);
#endif
void wifi_prepare_get_hw_spec_cmd(HostCmd_DS_COMMAND *cmd, t_u16 seq_number);

#ifdef OTP_CHANINFO
static void wlan_get_channel_region_cfg(void)
{
    uint32_t tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    wifi_prepare_get_channel_region_cfg_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_CHAN_REGION_CFG;
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

}
#endif

static void wlan_get_hw_spec(void)
{
    uint32_t tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);
    /* sdiopkt = outbuf */
    wifi_prepare_get_hw_spec_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_GET_HW_SPEC;
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

}

static void wlan_get_mac_addr_sta(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    wifi_prepare_get_mac_addr_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_802_11_MAC_ADDRESS;

    /* send CMD53 to write the command to get mac address */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

}

static void wlan_get_mac_addr_uap(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;
    t_u16 seq_number = 0;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);
    seq_number = HostCmd_SET_SEQ_NO_BSS_INFO(0 /* seq_num */, 0 /* bss_num */, MLAN_BSS_TYPE_UAP);

    /* sdiopkt = outbuf */
    wifi_prepare_get_mac_addr_cmd(&sdiopkt->hostcmd, seq_number);

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_802_11_MAC_ADDRESS;

    /* send CMD53 to write the command to get mac address */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

}

void wifi_prepare_get_fw_ver_ext_cmd(HostCmd_DS_COMMAND *cmd, int seq_number, int version_str_sel);
static void wlan_get_fw_ver_ext(int version_str_sel)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    wifi_prepare_get_fw_ver_ext_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num(), version_str_sel);

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_VERSION_EXT;

    /* send CMD53 to write the command to get mac address */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

}

void wifi_prepare_get_value1(HostCmd_DS_COMMAND *cmd, int seq_number);

static void wlan_get_value_1(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    wifi_prepare_get_value1(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_MAC_REG_ACCESS;

#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

}

void wifi_prepare_set_mac_addr_cmd(HostCmd_DS_COMMAND *cmd, int seq_number);
static void _wlan_set_mac_addr(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    wifi_prepare_set_mac_addr_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_802_11_MAC_ADDRESS;

    /* send CMD53 to write the command to get mac address */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();
}

static void wlan_set_11n_cfg(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, SDIO_OUTBUF_LEN);

    wrapper_wlan_cmd_11n_cfg(&sdiopkt->hostcmd);
    /* sdiopkt = outbuf */
    sdiopkt->hostcmd.seq_num = (t_u16)wlan_get_next_seq_num();
    sdiopkt->pkttype         = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent            = HostCmd_CMD_11N_CFG;

#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();
}

void wifi_prepare_enable_amsdu_cmd(HostCmd_DS_COMMAND *cmd, int seq_number);
static void wlan_enable_amsdu(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    wifi_prepare_enable_amsdu_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_AMSDU_AGGR_CTRL;

#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

}

static void wlan_cmd_shutdown(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    sdiopkt->hostcmd.command = HostCmd_CMD_FUNC_SHUTDOWN;
    sdiopkt->hostcmd.size    = (t_u16)S_DS_GEN;
    sdiopkt->hostcmd.seq_num = (t_u16)wlan_get_next_seq_num();
    sdiopkt->hostcmd.result  = 0;

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_FUNC_SHUTDOWN;

    (void)sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
}

void wlan_prepare_mac_control_cmd(HostCmd_DS_COMMAND *cmd, t_u16 seq_number);
static void wlan_set_mac_ctrl(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    wlan_prepare_mac_control_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_MAC_CONTROL;

    /* send CMD53 to write the command to set mac control */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

}

static void wlan_cmd_init(void)
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */
    sdiopkt->hostcmd.command = HostCmd_CMD_FUNC_INIT;
    sdiopkt->hostcmd.size    = (t_u16)S_DS_GEN;
    sdiopkt->hostcmd.seq_num = (t_u16)wlan_get_next_seq_num();
    sdiopkt->hostcmd.result  = 0;

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_FUNC_INIT;

#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();
}

#ifdef WLAN_LOW_POWER_ENABLE
void wifi_prepare_low_power_mode_cmd(HostCmd_DS_COMMAND *cmd, t_u16 seq_number);
static int wlan_set_low_power_mode()
{
    t_u32 tx_blocks = 1, buflen = MLAN_SDIO_BLOCK_SIZE;
    uint32_t resp;

    wifi_sdio_lock();

    (void)memset(outbuf, 0, buflen);

    /* sdiopkt = outbuf */

    wifi_prepare_low_power_mode_cmd(&sdiopkt->hostcmd, (t_u16)wlan_get_next_seq_num());

    sdiopkt->pkttype = MLAN_TYPE_CMD;
    sdiopkt->size    = sdiopkt->hostcmd.size + INTF_HEADER_LEN;

    last_cmd_sent = HostCmd_CMD_LOW_POWER_MODE;

    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);

    wifi_sdio_unlock();

    wifi_sdio_wait_for_cmdresp();

    return true;
}
#endif


/* Setup the firmware with commands */
static void wlan_fw_init_cfg(void)
{
#ifdef CONFIG_FW_VDLL
    mlan_private *pmpriv = (mlan_private *)mlan_adap->priv[0];
    mlan_adapter *pmadapter = pmpriv->adapter;
#endif

    wifi_io_d("FWCMD : INIT (0xa9)");

    wlan_cmd_init();

#ifdef WLAN_LOW_POWER_ENABLE
    if (low_power_mode)
    {
#ifdef CONFIG_FW_VDLL
        while (pmadapter->vdll_in_progress == MTRUE)
        {
            os_thread_sleep(os_msec_to_ticks(50));
        }
#endif
        wifi_io_d("CMD : LOW_POWER_MODE (0x128)");

        wlan_set_low_power_mode();
    }
#endif

    if (cal_data_valid)
    {
#ifdef CONFIG_FW_VDLL
        while (pmadapter->vdll_in_progress == MTRUE)
        {
            os_thread_sleep(os_msec_to_ticks(50));
        }
#endif
        wifi_io_d("CMD : SET_CAL_DATA (0x8f)");

        _wlan_set_cal_data();
    }

    /* When cal data set command is sent, fimrware looses alignment of SDIO Tx buffers.
     * So we need to send reconfigure command. This can be removed if fix is added in firmware.
     */
    wifi_io_d("CMD : RECONFIGURE_TX_BUFF (0xd9)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_reconfigure_tx_buffers();

    if (mac_addr_valid)
    {
        wifi_io_d("CMD : SET_MAC_ADDR (0x4d)");
#ifdef CONFIG_FW_VDLL
        while (pmadapter->vdll_in_progress == MTRUE)
        {
            os_thread_sleep(os_msec_to_ticks(50));
        }
#endif
        _wlan_set_mac_addr();
    }

#ifdef OTP_CHANINFO
    wifi_io_d("CMD : Channel Region CFG (0x0242)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_get_channel_region_cfg();
#endif

    wifi_io_d("CMD : GET_HW_SPEC (0x03)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_get_hw_spec();

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_get_value_1();

    wifi_io_d("CMD : GET_FW_VER_EXT (0x97)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_get_fw_ver_ext(0);

    wifi_io_d("CMD : GET_MAC_ADDR (0x4d)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_get_mac_addr_sta();

    wifi_io_d("CMD : GET_MAC_ADDR (0x4d)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_get_mac_addr_uap();

    wifi_io_d("CMD : GET_FW_VER_EXT (0x97)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_get_fw_ver_ext(3);

    wifi_io_d("CMD : MAC_CTRL (0x28)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_set_mac_ctrl();

    wifi_io_d("CMD : GET_FW_VER_EXT (0x97)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_get_fw_ver_ext(4);


    wifi_io_d("CMD : GET_FW_VER_EXT (0xcd)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_set_11n_cfg();

    wifi_io_d("CMD : GET_FW_VER_EXT (0xdf)");

#ifdef CONFIG_FW_VDLL
    while (pmadapter->vdll_in_progress == MTRUE)
    {
        os_thread_sleep(os_msec_to_ticks(50));
    }
#endif

    wlan_enable_amsdu();


    return;
}


#ifdef CONFIG_FW_VDLL
int wlan_send_sdio_vdllcmd(t_u8 *buf, t_u32 tx_blocks, t_u32 buflen)
{
    SDIOPkt *sdio = (SDIOPkt *)(void *)outbuf;
    uint32_t resp;

    (void)memcpy((void *)outbuf, (const void *)buf, tx_blocks * buflen);
    sdio->pkttype = MLAN_TYPE_VDLL;
    sdio->size    = sdio->hostcmd.size + INTF_HEADER_LEN;

#ifdef CONFIG_WIFI_IO_DUMP
    (void)PRINTF("OUT_CMD");
    dump_hex(outbuf, sdio->size);
#endif /* CONFIG_WIFI_IO_DUMP */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    return WM_SUCCESS;
}
#endif

int wlan_send_sdio_cmd(t_u8 *buf, t_u32 tx_blocks, t_u32 buflen)
{
    SDIOPkt *sdio = (SDIOPkt *)(void *)outbuf;
    uint32_t resp;

    (void)wifi_sdio_lock();

    (void)memcpy((void *)outbuf, (const void *)buf, tx_blocks * buflen);
    sdio->pkttype = MLAN_TYPE_CMD;
    sdio->size    = sdio->hostcmd.size + INTF_HEADER_LEN;

#ifdef CONFIG_WIFI_IO_DUMP
    (void)PRINTF("OUT_CMD");
    dump_hex(outbuf, sdio->size);
#endif /* CONFIG_WIFI_IO_DUMP */
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif

    last_cmd_sent = sdio->hostcmd.command;
    wifi_sdio_unlock();

    return WM_SUCCESS;
}

int wifi_send_cmdbuffer(t_u32 tx_blocks, t_u32 len)
{
    return wlan_send_sdio_cmd(ctrl_cmd_buf, tx_blocks, len);
}

#ifdef CONFIG_FW_VDLL
int wifi_send_vdllcmdbuffer(t_u32 tx_blocks, t_u32 len)
{
    return wlan_send_sdio_vdllcmd(vdll_cmd_buf, tx_blocks, len);
}
#endif

#ifdef CONFIG_WMM

#ifdef CONFIG_SDIO_MULTI_PORT_TX_AGGR
static t_u32 buf_block_len = 0;
static t_u8 start_port     = -1;
static t_u8 ports          = 0;
static t_u8 pkt_cnt        = 0;

/**
 *  @brief This function gets available SDIO port for writing data
 *
 *  @param pmadapter  A pointer to mlan_adapter structure
 *  @param pport      A pointer to port number
 *  @return           MLAN_STATUS_SUCCESS or MLAN_STATUS_FAILURE
 */
mlan_status wlan_get_wr_port_data(t_u8 *pport)
{
#ifdef CONFIG_WIFI_IO_DEBUG
    t_u32 wr_bitmap = mlan_adap->mp_wr_bitmap;
#endif

    ENTER();

    wifi_io_d("wlan_get_wr_port_data: mp_wr_bitmap=0x%08x", wr_bitmap);

    if ((1U << txportno) & mlan_adap->mp_wr_bitmap)
    {
        mlan_adap->mp_wr_bitmap &= (t_u32)(~(1 << txportno));
        *pport = txportno;
        txportno++;

        if (txportno == mlan_adap->mp_end_port)
        {
#if defined(SD8801)
            txportno = 1;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
            txportno = 0;
#endif
        }
    }
    else
    {
        LEAVE();
        return MLAN_STATUS_RESOURCE;
    }

#if defined(SD8801)
    if (*pport == CTRL_PORT)
    {
        wifi_d("Invalid data port=%d cur port=%d mp_wr_bitmap=0x%08x -> 0x%08x\r\n", *pport, txportno, wr_bitmap,
               mlan_adap->mp_wr_bitmap);
        LEAVE();
        return MLAN_STATUS_FAILURE;
    }
#endif

    wifi_io_d("port=%d mp_wr_bitmap=0x%08x -> 0x%08x\n", *pport, wr_bitmap, mlan_adap->mp_wr_bitmap);

    LEAVE();
    return MLAN_STATUS_SUCCESS;
}

static mlan_status wifi_tx_data(t_u8 start_port, t_u8 ports, t_u8 pkt_cnt, t_u32 txlen)
{
    t_u32 cmd53_port;
    t_u32 tx_blocks = 0, buflen = 0;
    uint32_t resp;
    bool ret;
#ifdef CONFIG_WIFI_FW_DEBUG
    int ret_cb;
#endif
#if defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    t_u32 port_count = 0;
#endif

    calculate_sdio_write_params(txlen, &tx_blocks, &buflen);

    if (pkt_cnt == 1)
    {
        cmd53_port = mlan_adap->ioport + start_port;
    }
    else
    {
#if defined(SD8801)
        cmd53_port = (mlan_adap->ioport | SDIO_MPA_ADDR_BASE | (ports << 4)) + start_port;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
        port_count = ports - 1U;
        cmd53_port = (mlan_adap->ioport | SDIO_MPA_ADDR_BASE | (port_count << 8)) + start_port;
#endif
    }

	if (ports >= 1) {
    	gsd_e("cmd53_port=%x, ports=%x, start_port=%x, pkt_cnt=%d, txlen=%d, txblocks=%d, wr=%x\r\n", cmd53_port, ports, start_port, pkt_cnt, txlen, tx_blocks, mlan_adap->mp_wr_bitmap);
	}

    /* send CMD53 */
    ret = sdio_drv_write(cmd53_port, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);

    if (ret == false)
    {
        wifi_io_e("sdio_drv_write failed (%d)", ret);
#ifdef CONFIG_WIFI_FW_DEBUG
        wifi_sdio_reg_dbg(NULL);
        if (wm_wifi.wifi_usb_mount_cb != NULL)
        {
            ret_cb = wm_wifi.wifi_usb_mount_cb();
            if (ret_cb == WM_SUCCESS)
                wifi_dump_firmware_info(NULL);
            else
                wifi_e("USB mounting failed");
        }
        else
            wifi_e("USB mount callback is not registered");
#endif
        return MLAN_STATUS_RESOURCE;
    }
    return MLAN_STATUS_SUCCESS;
}

mlan_status wlan_xmit_wmm_pkt(t_u8 interface, t_u32 txlen, t_u8 *tx_buf)
{
    t_u32 tx_blocks = 0, buflen = 0;
    mlan_status ret = MLAN_STATUS_SUCCESS;
    t_u8 port     = 0;

    if (buf_block_len == 0)
    {
        start_port = -1;
        ports      = 0;
        pkt_cnt    = 0;
    }

    calculate_sdio_write_params(txlen, &tx_blocks, &buflen);

#ifdef CONFIG_WIFI_IO_DEBUG
    (void)PRINTF("%s: txportno = %d mlan_adap->mp_wr_bitmap: %x\n\r", __func__, txportno, mlan_adap->mp_wr_bitmap);
#endif /* CONFIG_WIFI_IO_DEBUG */

    ret = wlan_get_wr_port_data(&port);
    if (ret != WM_SUCCESS)
    {
        return MLAN_STATUS_RESOURCE;
    }


    if (buf_block_len == 0)
    {
        start_port = port;
    }
#ifdef CONFIG_TX_RX_ZERO_COPY
	outbuf_t *buf = (outbuf_t *)tx_buf;
	struct pbuf *p = (struct pbuf *)(buf->buffer);
	t_u32 uCopied = 0;
	t_u32 header_len =  sizeof(TxPD) + INTF_HEADER_LEN + ETH_HDR_LEN;
	memcpy(outbuf + buf_block_len, (t_u8 *)&buf->intf_header[0], header_len);
	uCopied = pbuf_copy_partial(p, outbuf + buf_block_len + header_len, p->tot_len, 0);
#else
    memcpy(outbuf + buf_block_len, tx_buf, txlen);
#endif
    buf_block_len += tx_blocks * buflen;

    ports++;
    pkt_cnt++;


    wifi_io_info_d("OUT: i/f: %d, len: %d, start port: %d, pkt_cnt: %d, bufblocklen %d", interface, tx_blocks * buflen, start_port, pkt_cnt, buf_block_len);

    return MLAN_STATUS_SUCCESS;
}

mlan_status wlan_flush_wmm_pkt(t_u8 pkt_count)
{
    int ret;

    if (pkt_count == 0)
        return MLAN_STATUS_SUCCESS;

    w_pkt_d("Data TX: Driver=>FW, pkt_count %d", pkt_cnt);

    ret = wifi_tx_data(start_port, ports, pkt_cnt, buf_block_len);

    buf_block_len = 0;

    if (ret != MLAN_STATUS_SUCCESS)
    {
        return MLAN_STATUS_RESOURCE;
    }

    return MLAN_STATUS_SUCCESS;
}
#else
extern int retry_attempts;

static int get_free_port(void)
{
    /* Check if the port is available */
    if (!((1 << txportno) & mlan_adap->mp_wr_bitmap))
    {
#ifdef CONFIG_WIFI_IO_DEBUG
        wifi_io_e(
            "txportno out of sync txportno "
            "= (%d) mp_wr_bitmap = (0x%x)",
            txportno, mlan_adap->mp_wr_bitmap);
#endif /* CONFIG_WIFI_IO_DEBUG */

        return -WM_FAIL;
    }
    else
    {
        /* Mark the port number we will use */
        mlan_adap->mp_wr_bitmap &= ~(1 << txportno);
    }
    return WM_SUCCESS;
}

mlan_status wlan_xmit_wmm_pkt(t_u8 interface, t_u32 txlen, t_u8 *tx_buf)
{
    t_u32 tx_blocks = 0, buflen = 0;
    uint32_t resp;
    int ret   = false;
    int retry = retry_attempts;

    wifi_io_info_d("OUT: i/f: %d len: %d", interface, tx_blocks * buflen);

    calculate_sdio_write_params(txlen, &tx_blocks, &buflen);

#ifdef CONFIG_WIFI_IO_DEBUG
    (void)PRINTF("%s: txportno = %d mlan_adap->mp_wr_bitmap: %x\n\r", __func__, txportno, mlan_adap->mp_wr_bitmap);
#endif /* CONFIG_WIFI_IO_DEBUG */

retry_xmit:
    ret = get_free_port();
    if (ret == -WM_FAIL)
    {
        if (!retry)
        {
            ret = MLAN_STATUS_RESOURCE;
            goto exit_fn;
        }
        else
        {
            retry--;
            /* Allow the other thread to run and hence
             * update the write bitmap so that pkt
             * can be sent to FW */
            os_thread_sleep(os_msec_to_ticks(1));
            goto retry_xmit;
        }
    }


    /* send CMD53 */
    ret = sdio_drv_write(mlan_adap->ioport + txportno, 1, tx_blocks, buflen, tx_buf, &resp);

    txportno++;
    if (txportno == mlan_adap->mp_end_port)
    {
#if defined(SD8801)
        txportno = 1;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
        txportno = 0;
#endif
    }

    if (ret == false)
    {

        wifi_io_e("sdio_drv_write failed (%d)", ret);
#ifdef CONFIG_WIFI_FW_DEBUG
        wifi_sdio_reg_dbg(NULL);
        if (wm_wifi.wifi_usb_mount_cb != NULL)
        {
            ret = wm_wifi.wifi_usb_mount_cb();
            if (ret == WM_SUCCESS)
                wifi_dump_firmware_info(NULL);
            else
                wifi_e("USB mounting failed");
        }
        else
            wifi_e("USB mount callback is not registered");
#endif
        ret = MLAN_STATUS_RESOURCE;
        goto exit_fn;
    }


    ret = MLAN_STATUS_SUCCESS;

exit_fn:

    return ret;
}

mlan_status wlan_flush_wmm_pkt(t_u8 pkt_count)
{
    return MLAN_STATUS_SUCCESS;
}

#endif
#endif

mlan_status wlan_xmit_pkt(t_u8 *buffer, t_u32 txlen, t_u8 interface, t_u32 tx_control)
{
    t_u32 tx_blocks = 0, buflen = 0;
    uint32_t resp;
    bool ret;
#ifdef CONFIG_WIFI_FW_DEBUG
    int ret_cb;
#endif

    wifi_io_info_d("OUT: i/f: %d len: %d", interface, txlen);

    calculate_sdio_write_params(txlen, &tx_blocks, &buflen);

#ifdef CONFIG_WIFI_IO_DEBUG
    (void)PRINTF("%s: txportno = %d mlan_adap->mp_wr_bitmap: %x\n\r", __func__, txportno, mlan_adap->mp_wr_bitmap);
#endif /* CONFIG_WIFI_IO_DEBUG */
    /* Check if the port is available */
    if (!((1U << txportno) & mlan_adap->mp_wr_bitmap))
    {
        /*
         * fixme: This condition is triggered in legacy as well as
         * this new code. Check this out later.
         */
#ifdef CONFIG_WIFI_IO_DEBUG
        wifi_io_e(
            "txportno out of sync txportno "
            "= (%d) mp_wr_bitmap = (0x%x)",
            txportno, mlan_adap->mp_wr_bitmap);
#endif /* CONFIG_WIFI_IO_DEBUG */
        return MLAN_STATUS_RESOURCE;
    }
    else
    {
        /* Mark the port number we will use */
        mlan_adap->mp_wr_bitmap &= ~(1U << txportno);
    }

    process_pkt_hdrs((t_u8 *)buffer, txlen, interface, 0, tx_control);

    /* send CMD53 */
    ret = sdio_drv_write(mlan_adap->ioport + txportno, 1, tx_blocks, buflen, (t_u8 *)buffer, &resp);

    txportno++;
    if (txportno == mlan_adap->mp_end_port)
    {
#if defined(SD8801)
        txportno = 1;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
        txportno   = 0;
#endif
    }

    if (ret == false)
    {
        wifi_io_e("sdio_drv_write failed (%d)", ret);
#ifdef CONFIG_WIFI_FW_DEBUG
        wifi_sdio_reg_dbg(NULL);
        if (wm_wifi.wifi_usb_mount_cb != NULL)
        {
            ret_cb = wm_wifi.wifi_usb_mount_cb();
            if (ret_cb == WM_SUCCESS)
                wifi_dump_firmware_info(NULL);
            else
                wifi_e("USB mounting failed");
        }
        else
            wifi_e("USB mount callback is not registered");
#endif
        return MLAN_STATUS_RESOURCE;
    }
    return MLAN_STATUS_SUCCESS;
}

#ifdef CONFIG_WMM
mlan_status wlan_xmit_bypass_pkt(t_u8 *buffer, t_u32 txlen, t_u8 interface)
{
    t_u32 tx_blocks = 0, buflen = 0;
    uint32_t resp;
    bool ret;
#ifdef CONFIG_WIFI_FW_DEBUG
    int ret_cb;
#endif

    wifi_io_info_d("OUT: i/f: %d len: %d", interface, txlen);

    calculate_sdio_write_params(txlen, &tx_blocks, &buflen);

#ifdef CONFIG_WIFI_IO_DEBUG
    (void)PRINTF("%s: txportno = %d mlan_adap->mp_wr_bitmap: %x\n\r", __func__, txportno, mlan_adap->mp_wr_bitmap);
#endif /* CONFIG_WIFI_IO_DEBUG */
    /* Check if the port is available */
    if (!((1U << txportno) & mlan_adap->mp_wr_bitmap))
    {
        /*
         * fixme: This condition is triggered in legacy as well as
         * this new code. Check this out later.
         */
#ifdef CONFIG_WIFI_IO_DEBUG
        wifi_io_e(
            "txportno out of sync txportno "
            "= (%d) mp_wr_bitmap = (0x%x)",
            txportno, mlan_adap->mp_wr_bitmap);
#endif /* CONFIG_WIFI_IO_DEBUG */
        return MLAN_STATUS_RESOURCE;
    }
    else
    {
        /* Mark the port number we will use */
        mlan_adap->mp_wr_bitmap &= ~(1U << txportno);
    }

    /* send CMD53 */
    ret = sdio_drv_write(mlan_adap->ioport + txportno, 1, tx_blocks, buflen, (t_u8 *)buffer, &resp);

    txportno++;
    if (txportno == mlan_adap->mp_end_port)
    {
#if defined(SD8801)
        txportno = 1;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
        txportno   = 0;
#endif
    }

    if (ret == false)
    {
        wifi_io_e("sdio_drv_write failed (%d)", ret);
#ifdef CONFIG_WIFI_FW_DEBUG
        wifi_sdio_reg_dbg(NULL);
        if (wm_wifi.wifi_usb_mount_cb != NULL)
        {
            ret_cb = wm_wifi.wifi_usb_mount_cb();
            if (ret_cb == WM_SUCCESS)
                wifi_dump_firmware_info(NULL);
            else
                wifi_e("USB mounting failed");
        }
        else
            wifi_e("USB mount callback is not registered");
#endif
        return MLAN_STATUS_RESOURCE;
    }
    return MLAN_STATUS_SUCCESS;
}
#endif

/*
 * This function gets interrupt status.
 */
t_void wlan_interrupt(mlan_adapter *pmadapter)
{
    /* Read SDIO multiple port group registers */
    uint32_t resp = 0;
    int ret;
    t_u8 *mp_regs = pmadapter->mp_regs;

    /* Read the registers in DMA aligned buffer */
#if defined(SD8801)
    ret = sdio_drv_read(0, 1, 1, MAX_MP_REGS, mp_regs, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    ret = sdio_drv_read(REG_PORT | MLAN_SDIO_BYTE_MODE_MASK, 1, 1, MAX_MP_REGS, mp_regs, &resp);
#endif

    if (!ret)
    {
        return;
    }

    t_u8 sdio_ireg = mp_regs[HOST_INT_STATUS_REG];

    if (sdio_ireg != 0U)
    {
        /*
         * DN_LD_HOST_INT_STATUS and/or UP_LD_HOST_INT_STATUS
         * Clear the interrupt status register
         */
        pmadapter->sdio_ireg |= sdio_ireg;
    }

#ifdef CONFIG_WMM
    t_u32 wr_bitmap;

#if defined(SD8801)
    wr_bitmap = ((t_u16)mp_regs[WR_BITMAP_U]) << 8;
    wr_bitmap |= (t_u16)mp_regs[WR_BITMAP_L];
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    wr_bitmap = (t_u32)mp_regs[WR_BITMAP_L];
    wr_bitmap |= ((t_u32)mp_regs[WR_BITMAP_U]) << 8;
    wr_bitmap |= ((t_u32)mp_regs[WR_BITMAP_1L]) << 16;
    wr_bitmap |= ((t_u32)mp_regs[WR_BITMAP_1U]) << 24;
#endif

    if (!!wr_bitmap)
    {
        if (mlan_adap->wait_txbuf == true)
        {
            os_semaphore_put(&txbuf_sem);
            send_wifi_driver_tx_data_event(0);
        }
    }
#endif

#ifdef CONFIG_WIFI_IO_DEBUG
    t_u32 rd_bitmap;
#ifndef CONFIG_WMM
    t_u32 wr_bitmap;

#if defined(SD8801)
    wr_bitmap = ((t_u16)mp_regs[WR_BITMAP_U]) << 8;
    wr_bitmap |= (t_u16)mp_regs[WR_BITMAP_L];
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    wr_bitmap = (t_u32)mp_regs[WR_BITMAP_L];
    wr_bitmap |= ((t_u32)mp_regs[WR_BITMAP_U]) << 8;
    wr_bitmap |= ((t_u32)mp_regs[WR_BITMAP_1L]) << 16;
    wr_bitmap |= ((t_u32)mp_regs[WR_BITMAP_1U]) << 24;
#endif

#endif
#if defined(SD8801)
    rd_bitmap = ((t_u16)mp_regs[RD_BITMAP_U]) << 8;
    rd_bitmap |= (t_u16)mp_regs[RD_BITMAP_L];
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    rd_bitmap = (t_u32)mp_regs[RD_BITMAP_L];
    rd_bitmap |= ((t_u32)mp_regs[RD_BITMAP_U]) << 8;
    rd_bitmap |= ((t_u32)mp_regs[RD_BITMAP_1L]) << 16;
    rd_bitmap |= ((t_u32)mp_regs[RD_BITMAP_1U]) << 24;
#endif

    (void)PRINTF("INT : rd_bitmap=0x%x\n\r", rd_bitmap);

    (void)PRINTF("INT : wr_bitmap=0x%x\n\r", wr_bitmap);

    (void)PRINTF("INT : sdio_ireg = (0x%x)\r\n", sdio_ireg);
	(void)PRINTF("=============================================\r\n");
#endif /* CONFIG_WIFI_IO_DEBUG */
}

#ifdef CONFIG_TP_OPTIMIZATIONS
static t_u32 bitcount(t_u32 num)
{
    t_u32 count = 0;
    static t_u32 nibblebits[] = { 0, 1, 1, 2, 1, 2, 2, 3,
                    1, 2, 2, 3, 2, 3, 3, 4 };
    for (; num != 0; num >>= 4)
        count += nibblebits[num & 0x0f];
    return count;
}
#endif
#ifdef CONFIG_SDIO_MULTI_PORT_RX_AGGR
#ifdef CONFIG_TP_OPTIMIZATIONS
static t_u8 start_check = 0;
static t_u8 skip_int = 0;
#endif

/* returns port number from rd_bitmap. if ctrl port, then it clears
 * the bit and does nothing else
 * if data port then increments curr_port value also */
static mlan_status wlan_get_rd_port(mlan_adapter *pmadapter, t_u32 *pport, t_u32 *rxlen, t_u32 *rxblocks, bool *aggr)
{
#if defined(SD8801)
    t_u16 rd_bitmap = pmadapter->mp_rd_bitmap;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    t_u32 rd_bitmap = pmadapter->mp_rd_bitmap;
#endif
    t_u16 pkt_cnt = 0;
    t_u32 len_reg_l, len_reg_u;
    t_u32 rx_len;
    t_u32 rx_blocks;
    t_u16 ports      = 0;
    t_u32 start_port = -1;
    t_u32 cmd53_port = 0;
#if defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    t_u32 port_count = 0;
#endif

    *pport    = -1;
    *rxlen    = 0;
    *rxblocks = 0;

    wifi_io_d(
        "wlan_get_rd_port: mp_rd_bitmap=0x%x"
        " curr_rd_bitmap=0x%x",
        pmadapter->mp_rd_bitmap, pmadapter->curr_rd_port);

#if defined(SD8801)
    if (!(rd_bitmap & (CTRL_PORT_MASK | DATA_PORT_MASK)))
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    if (!(rd_bitmap & DATA_PORT_MASK))
#endif
    {
        return MLAN_STATUS_FAILURE;
    }

#if defined(SD8801)
    if (pmadapter->mp_rd_bitmap & CTRL_PORT_MASK)
    {
        pmadapter->mp_rd_bitmap &= (t_u32)(~CTRL_PORT_MASK);
        *pport = CTRL_PORT;

        len_reg_l = RD_LEN_P0_L + (*pport << 1);
        len_reg_u = RD_LEN_P0_U + (*pport << 1);
        rx_len    = ((t_u16)pmadapter->mp_regs[len_reg_u]) << 8;
        rx_len |= (t_u16)pmadapter->mp_regs[len_reg_l];
        rx_blocks = (rx_len + MLAN_SDIO_BLOCK_SIZE - 1) / MLAN_SDIO_BLOCK_SIZE;
        rx_len    = (t_u16)(rx_blocks * MLAN_SDIO_BLOCK_SIZE);
        *rxlen    = rx_len;
        *rxblocks = rx_blocks;

        *pport = mlan_adap->ioport + *pport;

        wifi_io_d("RX: port=%d rxlen=%u rx_blocks=%u\r\n", *pport, *rxlen, *rxblocks);

        wifi_io_d("wlan_get_rd_port: port=%d mp_rd_bitmap=0x%04x", *pport, pmadapter->mp_rd_bitmap);
    }
    else
    {
#endif

#ifdef CONFIG_TP_OPTIMIZATIONS
        /* This change is being added to handle low throughput caused for Tx due to excess interrupts received for Rx.
        * The intention is to ignore the interrupt if received agreggated packets are less than 4.
        * However, there could be single data packets during DHCP request, etc. whose Rx interrupt should not be skipped.
        * For actual data, we are checking if read bitmap has at least 4 ports available if current read port is 27.
        * If current read port is greater than 27 then any number of available ports is fine. */
        if (bitcount(pmadapter->mp_rd_bitmap) == 8U)
        {
            start_check = 1;
        }

        if (skip_int == 2)
        {
            start_check = 0;
        }
        /* Data */
        if ((start_check) && ((pmadapter->mp_rd_bitmap & ((pmadapter->curr_rd_port > 27 ? 1U : 0xFU) << pmadapter->curr_rd_port)) == 0U))
        {
            skip_int++;
            return MLAN_STATUS_FAILURE;
        }
#endif
        while ((pmadapter->mp_rd_bitmap & (1U << pmadapter->curr_rd_port)) != 0U)
        {
            *pport = pmadapter->curr_rd_port;

            len_reg_l = RD_LEN_P0_L + (*pport << 1U);
            len_reg_u = RD_LEN_P0_U + (*pport << 1U);
            rx_len    = ((t_u16)pmadapter->mp_regs[len_reg_u]) << 8;
            rx_len |= (t_u16)pmadapter->mp_regs[len_reg_l];
            rx_blocks = (rx_len + MLAN_SDIO_BLOCK_SIZE - 1U) / MLAN_SDIO_BLOCK_SIZE;
            rx_len    = (t_u16)(rx_blocks * MLAN_SDIO_BLOCK_SIZE);
            if ((*rxlen + rx_len) > INBUF_SIZE)
            {
                break;
            }

            pmadapter->mp_rd_bitmap &=
#if defined(SD8801)
                (t_u16)(~(1 << pmadapter->curr_rd_port));
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
            (t_u32)(~(1 << pmadapter->curr_rd_port));
#endif

#if defined(SD8801)
            if (!pkt_cnt)
                start_port = *pport;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
		    if (start_port == -1)
		    {
		        start_port = *pport;
		    }
#endif

#if defined(SD8801)
            if (start_port <= *pport)
            {
                ports |= (1 << (pkt_cnt));
            }
            else
            {
                ports |= (1 << (pkt_cnt + 1));
            }
#endif

            *rxlen += rx_len;
            *rxblocks += rx_blocks;

            wifi_io_d("RX: port=%d rxlen=%u rx_blocks=%u\r\n", *pport, *rxlen, *rxblocks);

            if (++pmadapter->curr_rd_port == MAX_PORT)
            {
#if defined(SD8801)
                pmadapter->curr_rd_port = 1;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
            pmadapter->curr_rd_port = 0;
#endif
            }

            pkt_cnt++;
#if defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
            ports++;
#endif

            if (pkt_cnt == SDIO_MP_AGGR_DEF_PKT_LIMIT_MAX)
            {
                break;
            }
#if defined(SD8801)
            if ((pmadapter->curr_rd_port < start_port) &&
                (((MAX_PORT - start_port) + pmadapter->curr_rd_port) >= SDIO_MP_AGGR_DEF_PKT_LIMIT))
            {
                break;
            }
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
	        if (((pmadapter->curr_rd_port < start_port) &&
	             (((MAX_PORT - start_port) + pmadapter->curr_rd_port) >= (pmadapter->mp_end_port >> 1))) ||
	            ((pmadapter->curr_rd_port - start_port) >= (pmadapter->mp_end_port >> 1)))
	        {
	            break;
	        }
#endif
        }

        if (*pport == -1)
        {
            wifi_io_e("wlan_get_rd_port : Returning FAILURE");
            return MLAN_STATUS_FAILURE;
        }

#if defined(SD8801)
        if (pkt_cnt > 1)
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    	if (ports > 1U)
#endif
        {
#if defined(SD8801)
            cmd53_port = (pmadapter->ioport | SDIO_MPA_ADDR_BASE | (ports << 4)) + start_port;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
        port_count = ports - 1U;
        cmd53_port = (pmadapter->ioport | SDIO_MPA_ADDR_BASE | (port_count << 8)) + start_port;
#endif
            *pport = cmd53_port;

            *aggr = true;
            // (void)PRINTF("cmd53_port=%x, ports=%x, start_port=%x rxlen=%d rxblocks=%d\r\n", cmd53_port, ports,
            // start_port, *rxlen, *rxblocks);
        }
        else
        {
            *pport = mlan_adap->ioport + *pport;
        }

        wifi_io_d("port=%x mp_rd_bitmap=0x%x -> 0x%x\n", *pport, rd_bitmap, pmadapter->mp_rd_bitmap);
#if defined(SD8801)
    }
#endif

    return MLAN_STATUS_SUCCESS;
}

/*
 * Assumes that pmadapter->mp_rd_bitmap contains latest values
 */
static mlan_status _handle_sdio_packet_read(mlan_adapter *pmadapter, t_u8 **packet, t_u32 *datalen, t_u32 *pkt_type)
{
    t_u32 port;
    t_u32 rx_len = 0, rx_blocks = 0;
    bool aggr = false;

    mlan_status ret = wlan_get_rd_port(pmadapter, &port, &rx_len, &rx_blocks, &aggr);

    /* nothing to read */
    if (ret != MLAN_STATUS_SUCCESS)
    {
        return ret;
    }

    *datalen = rx_len;
	//PRINTF("rx len: %d\r\n", rx_len);
    *packet = wlan_read_rcv_packet(port, rx_len, rx_blocks, pkt_type, aggr);

    if ((*packet) == MNULL)
    {
        return MLAN_STATUS_FAILURE;
    }

    return MLAN_STATUS_SUCCESS;
}
#else
/* returns port number from rd_bitmap. if ctrl port, then it clears
 * the bit and does nothing else
 * if data port then increments curr_port value also */
static mlan_status wlan_get_rd_port(mlan_adapter *pmadapter, t_u32 *pport)
{
#if defined(SD8801)
    t_u16 rd_bitmap = pmadapter->mp_rd_bitmap;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    t_u32 rd_bitmap = pmadapter->mp_rd_bitmap;
#endif

    wifi_io_d(
        "wlan_get_rd_port: mp_rd_bitmap=0x%x"
        " curr_rd_bitmap=0x%x",
        pmadapter->mp_rd_bitmap, pmadapter->curr_rd_port);

#if defined(SD8801)
    if (!(rd_bitmap & (CTRL_PORT_MASK | DATA_PORT_MASK)))
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    if (!(rd_bitmap & DATA_PORT_MASK))
#endif
        return MLAN_STATUS_FAILURE;

#if defined(SD8801)
    if (pmadapter->mp_rd_bitmap & CTRL_PORT_MASK)
    {
        pmadapter->mp_rd_bitmap &= (t_u16)(~CTRL_PORT_MASK);
        *pport = CTRL_PORT;

        wifi_io_d("wlan_get_rd_port: port=%d mp_rd_bitmap=0x%04x", *pport, pmadapter->mp_rd_bitmap);
    }
    else
    {
#endif
        /* Data */
        if ((pmadapter->mp_rd_bitmap & (1 << pmadapter->curr_rd_port)) != 0U)
        {
            pmadapter->mp_rd_bitmap &=
#if defined(SD8801)
                (t_u16)(~(1 << pmadapter->curr_rd_port));
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
            (t_u32)(~(1 << pmadapter->curr_rd_port));
#endif
            *pport = pmadapter->curr_rd_port;

            if (++pmadapter->curr_rd_port == MAX_PORT)
#if defined(SD8801)
                pmadapter->curr_rd_port = 1;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
            pmadapter->curr_rd_port = 0;
#endif
        }
        else
        {
            wifi_io_e("wlan_get_rd_port : Returning FAILURE");
            return MLAN_STATUS_FAILURE;
        }

        wifi_io_d("port=%d mp_rd_bitmap=0x%x -> 0x%x\n", *pport, rd_bitmap, pmadapter->mp_rd_bitmap);
#if defined(SD8801)
    }
#endif

    return MLAN_STATUS_SUCCESS;
}

/*
 * Assumes that pmadapter->mp_rd_bitmap contains latest values
 */
static mlan_status _handle_sdio_packet_read(mlan_adapter *pmadapter, t_u8 **packet, t_u32 *datalen, t_u32 *pkt_type)
{
    t_u32 port;
    t_u32 rx_len, rx_blocks;

    mlan_status ret = wlan_get_rd_port(pmadapter, &port);

    /* nothing to read */
    if (ret != MLAN_STATUS_SUCCESS)
        return ret;

    t_u32 len_reg_l = RD_LEN_P0_L + (port << 1);
    t_u32 len_reg_u = RD_LEN_P0_U + (port << 1);

    rx_len = ((t_u16)pmadapter->mp_regs[len_reg_u]) << 8;
    *datalen = rx_len |= (t_u16)pmadapter->mp_regs[len_reg_l];

    rx_blocks = (rx_len + MLAN_SDIO_BLOCK_SIZE - 1) / MLAN_SDIO_BLOCK_SIZE;
    rx_len = (t_u16)(rx_blocks * MLAN_SDIO_BLOCK_SIZE);

    port = mlan_adap->ioport + port;
	
    *packet = wlan_read_rcv_packet(port, rx_len, rx_blocks, pkt_type, false);

    if (!*packet)
        return MLAN_STATUS_FAILURE;

    return MLAN_STATUS_SUCCESS;
}
#endif

#if defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
static mlan_status _handle_sdio_cmd_read(
    mlan_adapter *pmadapter, t_u8 **packet, t_u32 *datalen, t_u32 *pkt_type, t_u32 rx_len, t_u32 rx_blocks)
{
    *datalen = rx_len;

    *packet = wlan_read_rcv_packet(mlan_adap->ioport | CMD_PORT_SLCT, rx_len, rx_blocks, pkt_type, false);

    if ((*packet) == MNULL)
    {
        return MLAN_STATUS_FAILURE;
    }

    return MLAN_STATUS_SUCCESS;
}
/*
 * This function keeps on looping till all the packets are read
 */
static void handle_sdio_cmd_read(mlan_adapter *pmadapter, t_u32 rx_len, t_u32 rx_blocks)
{
    t_u32 datalen = 0;

    t_u32 pkt_type = 0;
    t_u8 *packet   = NULL;
#ifdef CONFIG_RX_POOL
	inbuf_t *pbuf = NULL;

    (void)_handle_sdio_cmd_read(pmadapter, (uint8_t**)&pbuf, &datalen, &pkt_type, rx_len, rx_blocks);
    if (pkt_type == MLAN_TYPE_CMD || pkt_type == MLAN_TYPE_EVENT)
    {
    	packet = pbuf->data;
        /* non-data packets such as events
           and command responses are
           handled here */
        (void)wlan_decode_rx_packet(packet, pkt_type);
    }
	if (pbuf != NULL) {
		 wifi_wmm_inbuf_put(pbuf);
	}
#else
	(void)_handle_sdio_cmd_read(pmadapter, &packet, &datalen, &pkt_type, rx_len, rx_blocks);
    if (pkt_type == MLAN_TYPE_CMD || pkt_type == MLAN_TYPE_EVENT)
    {
        /* non-data packets such as events
           and command responses are
           handled here */
        (void)wlan_decode_rx_packet(packet, pkt_type);
    }
#endif
}
#endif

/*
 * This function keeps on looping till all the packets are read
 */
static void handle_sdio_packet_read(mlan_adapter *pmadapter)
{
    mlan_status ret = MLAN_STATUS_SUCCESS;
    t_u32 datalen   = 0;

    pmadapter->mp_rd_bitmap =
#if defined(SD8801)
        ((t_u16)pmadapter->mp_regs[RD_BITMAP_U]) << 8;
    pmadapter->mp_rd_bitmap |= (t_u16)pmadapter->mp_regs[RD_BITMAP_L];
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
        (t_u32)pmadapter->mp_regs[RD_BITMAP_L];
    pmadapter->mp_rd_bitmap |= ((t_u32)pmadapter->mp_regs[RD_BITMAP_U]) << 8;
    pmadapter->mp_rd_bitmap |= ((t_u32)pmadapter->mp_regs[RD_BITMAP_1L]) << 16;
    pmadapter->mp_rd_bitmap |= ((t_u32)pmadapter->mp_regs[RD_BITMAP_1U]) << 24;
#endif

    while (true)
    {
        t_u32 pkt_type;
        t_u32 rx_blocks;
        t_u32 size       = 0;
        t_u32 total_size = 0;
        t_u8 *packet     = NULL;
        t_u8 interface;
#ifdef CONFIG_RX_POOL
		inbuf_t *pbuf = NULL;

        ret = _handle_sdio_packet_read(pmadapter, (uint8_t**)&pbuf, &datalen, &pkt_type);
        if (ret != MLAN_STATUS_SUCCESS)
        {
        	if (pbuf != NULL)
	        	wifi_wmm_inbuf_put(pbuf);
            /* nothing to read. break out of while loop */
            break;
        }
		//xQueueSend(wm_wifi.io_rx, &pbuf);
		if (pbuf != NULL) {
			pbuf->data_len = datalen;
			pbuf->pkt_type = pkt_type;
			//PRINTF("len = %d %x\r\n", pbuf->data_len, pbuf);
			if( xQueueSend( wm_wifi.io_rx, ( void * ) &pbuf, ( TickType_t ) 10 ) != pdPASS ) {
				if (pbuf != NULL)
		        	wifi_wmm_inbuf_put(pbuf);
			}
		}
#else
		ret = _handle_sdio_packet_read(pmadapter, &packet, &datalen, &pkt_type);
        if (ret != MLAN_STATUS_SUCCESS)
        {
            /* nothing to read. break out of while loop */
            break;
        }

        if (pkt_type == MLAN_TYPE_DATA)
        {
            while (total_size < datalen)
            {
                SDIOPkt *insdiopkt = (SDIOPkt *)(void *)packet;
                size               = insdiopkt->size;
                pkt_type           = insdiopkt->pkttype;

                rx_blocks = (size + MLAN_SDIO_BLOCK_SIZE - 1U) / MLAN_SDIO_BLOCK_SIZE;
                size      = (t_u16)(rx_blocks * MLAN_SDIO_BLOCK_SIZE);

                interface = *((t_u8 *)packet + INTF_HEADER_LEN);

                wifi_io_info_d("IN: i/f: %d len: %d", interface, size);

                if (!size)
                {
                    break;
                }

                if (bus.wifi_low_level_input != NULL)
                {
                    (void)bus.wifi_low_level_input(interface, packet, size);
                }

                packet += size;
                total_size += size;
            }
        }
        else
        {
            /* non-data packets such as events
               and command responses are
               handled here */
            (void)wlan_decode_rx_packet(packet, pkt_type);
        }
#endif
    }
}

/*
 * This is supposed to be called in thread context.
 */
mlan_status wlan_process_int_status(mlan_adapter *pmadapter)
{
    mlan_status ret = MLAN_STATUS_SUCCESS;
#if defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    t_u8 cmd_rd_len_0 = CMD_RD_LEN_0;
    t_u8 cmd_rd_len_1 = CMD_RD_LEN_1;
    t_u32 rx_len;
    t_u32 rx_blocks;
#endif

    /* Get the interrupt status */
    wlan_interrupt(pmadapter);

    t_u8 sdio_ireg       = pmadapter->sdio_ireg;
    pmadapter->sdio_ireg = 0;

    if (!sdio_ireg)
    {
        goto done;
    }

#if defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    /* check the command port */
    if ((sdio_ireg & DN_LD_CMD_PORT_HOST_INT_STATUS) != 0U)
    {
        if (pmadapter->cmd_sent != 0U)
        {
            pmadapter->cmd_sent = MFALSE;
        }
        PRINTM(MINFO, "cmd_sent=%d\n", pmadapter->cmd_sent);
    }

    if ((sdio_ireg & UP_LD_CMD_PORT_HOST_INT_STATUS) != 0U)
    {
        /* read the len of control packet */
        rx_len = ((t_u32)pmadapter->mp_regs[cmd_rd_len_1]) << 8;
        rx_len |= (t_u32)pmadapter->mp_regs[cmd_rd_len_0];
        PRINTM(MINFO, "RX: cmd port rx_len=%u\n", rx_len);

        rx_blocks = (rx_len + MLAN_SDIO_BLOCK_SIZE - 1U) / MLAN_SDIO_BLOCK_SIZE;

        // (void)PRINTF("CMD: cmd port rx_len=%u rx_blocks=%u\r\n", rx_len, rx_blocks);
        rx_len = (t_u32)(rx_blocks * MLAN_SDIO_BLOCK_SIZE);

        handle_sdio_cmd_read(pmadapter, rx_len, rx_blocks);
    }
#elif defined(SD8801)
    /*
     * Below two statement look like they are present for the purpose
     * of unconditional initializing of mp_wr_bitmap which will be used
     * during packet xmit. proper mlan code does not do this most
     * probably because they have used wlan_get_wr_port_data() to
     * decide on the write port which we have not done. Check this out
     * later.
     */
    pmadapter->mp_wr_bitmap = ((t_u16)pmadapter->mp_regs[WR_BITMAP_U]) << 8;
    pmadapter->mp_wr_bitmap |= (t_u16)pmadapter->mp_regs[WR_BITMAP_L];
#endif

    pmadapter->mp_wr_bitmap =
#if defined(SD8801)
        ((t_u16)pmadapter->mp_regs[WR_BITMAP_U]) << 8;
    pmadapter->mp_wr_bitmap |= (t_u16)pmadapter->mp_regs[WR_BITMAP_L];
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
        (t_u32)pmadapter->mp_regs[WR_BITMAP_L];
    pmadapter->mp_wr_bitmap |= ((t_u32)pmadapter->mp_regs[WR_BITMAP_U]) << 8;
    pmadapter->mp_wr_bitmap |= ((t_u32)pmadapter->mp_regs[WR_BITMAP_1L]) << 16;
    pmadapter->mp_wr_bitmap |= ((t_u32)pmadapter->mp_regs[WR_BITMAP_1U]) << 24;
#endif

    /*
     * DN_LD_HOST_INT_STATUS interrupt happens when the txmit sdio
     * ports are freed This is usually when we write to port most
     * significant port.
     * 当我们写port=0x1f的时候，会有这个中断出现
     */
    if ((sdio_ireg & DN_LD_HOST_INT_STATUS) != 0U)
    {
        pmadapter->mp_wr_bitmap =
#if defined(SD8801)
            ((t_u16)pmadapter->mp_regs[WR_BITMAP_U]) << 8;
        pmadapter->mp_wr_bitmap |= (t_u16)pmadapter->mp_regs[WR_BITMAP_L];
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
            (t_u32)pmadapter->mp_regs[WR_BITMAP_L];
        pmadapter->mp_wr_bitmap |= ((t_u32)pmadapter->mp_regs[WR_BITMAP_U]) << 8;
        pmadapter->mp_wr_bitmap |= ((t_u32)pmadapter->mp_regs[WR_BITMAP_1L]) << 16;
        pmadapter->mp_wr_bitmap |= ((t_u32)pmadapter->mp_regs[WR_BITMAP_1U]) << 24;
#endif
		gsd_e("INT : wd_bitmap=0x%x\n\r", pmadapter->mp_wr_bitmap);
    }

    /*
     * As firmware will not generate download ready interrupt if the
     * port updated is command port only, cmd_sent should be done for
     * any SDIO interrupt.
     */
#if defined(SD8801)
    if (pmadapter->cmd_sent == true)
    {
        /*
         * Check if firmware has attach buffer at command port and
         * update just that in wr_bit_map.
         */
        pmadapter->mp_wr_bitmap |= (t_u16)pmadapter->mp_regs[WR_BITMAP_L] & CTRL_PORT_MASK;

        if (pmadapter->mp_wr_bitmap & CTRL_PORT_MASK)
            pmadapter->cmd_sent = false;
    }
#endif

    if ((sdio_ireg & UP_LD_HOST_INT_STATUS) != 0U)
    {
        /* This means there is data to be read */
		perf_start(4);
        handle_sdio_packet_read(pmadapter);
		perf_end(4);
    }

    ret = MLAN_STATUS_SUCCESS;

done:
    return ret;
}

#ifdef CONFIG_RX_POOL
mlan_status wlan_process_rx(mlan_adapter *pmadapter, inbuf_t *pbuf)
{
	t_u32 pkt_type;
	t_u32 rx_blocks;
	t_u32 size		 = 0;
	t_u32 total_size = 0;
	t_u8 *packet	 = NULL;
	t_u8 interface;
	u32_t datalen = 0;
	packet = pbuf->data;
	datalen = pbuf->data_len;
	pkt_type = pbuf->pkt_type;
	//PRINTF("datalen2 : %d %x \r\n", pbuf->data_len, pbuf);
    if (pkt_type == MLAN_TYPE_DATA) {
        while (total_size < datalen) {
            SDIOPkt *insdiopkt = (SDIOPkt *)(void *)packet;
            size               = insdiopkt->size;
            pkt_type           = insdiopkt->pkttype;

            rx_blocks = (size + MLAN_SDIO_BLOCK_SIZE - 1U) / MLAN_SDIO_BLOCK_SIZE;
            size      = (t_u16)(rx_blocks * MLAN_SDIO_BLOCK_SIZE);

            interface = *((t_u8 *)packet + INTF_HEADER_LEN);

            wifi_io_info_d("IN: i/f: %d len: %d", interface, size);

            if (!size)
            {
                break;
            }

            if (bus.wifi_low_level_input != NULL)
            {
                (void)bus.wifi_low_level_input(interface, packet, size);
            }

            packet += size;
            total_size += size;
        }
    } else {
        /* non-data packets such as events
           and command responses are
           handled here */
        (void)wlan_decode_rx_packet(packet, pkt_type);
    }
	if (pbuf != NULL)
		wifi_wmm_inbuf_put(pbuf);

	return MLAN_STATUS_SUCCESS;
}
#endif

/**
 * Interrupt callback handler registered with the SDIO driver.
 */
void handle_cdint(int error)
{
    /* Wake up LWIP thread. */
    if ((wifi_core_thread != MNULL) && !error && g_txrx_flag)
    {
        g_txrx_flag = false;
        (void)os_event_notify_put(wifi_core_thread);
    }
}

int wifi_raw_packet_recv(t_u8 **data, t_u32 *pkt_type)
{
    if (data == MNULL)
    {
        return -WM_FAIL;
    }

    wifi_core_thread = os_get_current_task_handle();

    int sta = (int)os_enter_critical_section();
    /* Allow interrupt handler to deliver us a packet */
    g_txrx_flag = true;

    sdio_enable_interrupt();

    os_exit_critical_section((unsigned long)sta);

    /* Wait till we receive a packet from SDIO */
    (void)os_event_notify_get(OS_WAIT_FOREVER);

    /* Get the interrupt status */
    wlan_interrupt(mlan_adap);

    t_u8 sdio_ireg       = mlan_adap->sdio_ireg;
    mlan_adap->sdio_ireg = 0;

#if defined(SD8801)
    if (!(sdio_ireg & UP_LD_HOST_INT_STATUS))
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    if (!(sdio_ireg & UP_LD_CMD_PORT_HOST_INT_STATUS))
#endif
    {
        return -WM_FAIL;
    }

    t_u32 datalen = 0;

    mlan_adap->mp_rd_bitmap =
#if defined(SD8801)
        ((t_u16)mlan_adap->mp_regs[RD_BITMAP_U]) << 8;
    mlan_adap->mp_rd_bitmap |= (t_u16)mlan_adap->mp_regs[RD_BITMAP_L];
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
        (t_u32)mlan_adap->mp_regs[RD_BITMAP_L];
    mlan_adap->mp_rd_bitmap |= ((t_u32)mlan_adap->mp_regs[RD_BITMAP_U]) << 8;
    mlan_adap->mp_rd_bitmap |= ((t_u32)mlan_adap->mp_regs[RD_BITMAP_1L]) << 16;
    mlan_adap->mp_rd_bitmap |= ((t_u32)mlan_adap->mp_regs[RD_BITMAP_1U]) << 24;
#endif

    t_u8 *packet = NULL;

#if defined(SD8801)
    int ret;
    while (1)
    {
        ret = _handle_sdio_packet_read(mlan_adap, &packet, &datalen, pkt_type);
        if (ret == MLAN_STATUS_SUCCESS)
        {
            break;
        }
    }
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)

    t_u8 cmd_rd_len_0 = CMD_RD_LEN_0;
    t_u8 cmd_rd_len_1 = CMD_RD_LEN_1;
    t_u32 rx_len;
    t_u32 rx_blocks;

    /* read the len of control packet */
    rx_len = ((t_u32)mlan_adap->mp_regs[cmd_rd_len_1]) << 8;
    rx_len |= (t_u32)mlan_adap->mp_regs[cmd_rd_len_0];
    PRINTM(MINFO, "RX: cmd port rx_len=%u\n", rx_len);

    rx_blocks = (rx_len + MLAN_SDIO_BLOCK_SIZE - 1U) / MLAN_SDIO_BLOCK_SIZE;

    rx_len = (t_u32)(rx_blocks * MLAN_SDIO_BLOCK_SIZE);

    (void)_handle_sdio_cmd_read(mlan_adap, &packet, &datalen, pkt_type, rx_len, rx_blocks);
#endif
    *data = packet;
    return WM_SUCCESS;
}

int wifi_raw_packet_send(const t_u8 *packet, t_u32 length)
{
    if ((packet == MNULL) || !length)
    {
        return -WM_E_INVAL;
    }

    if (length > SDIO_OUTBUF_LEN)
    {
        wifi_io_e("Insufficient buffer");
        return -WM_FAIL;
    }

    t_u32 tx_blocks = 0, buflen = 0;
    calculate_sdio_write_params(length, &tx_blocks, &buflen);

    (void)memcpy((void *)outbuf, (const void *)packet, length);
#ifdef CONFIG_WIFI_IO_DUMP
    dump_hex(outbuf, length);
#endif
    uint32_t resp;
#if defined(SD8801)
    sdio_drv_write(mlan_adap->ioport, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_write(mlan_adap->ioport | CMD_PORT_SLCT, 1, tx_blocks, buflen, (t_u8 *)outbuf, &resp);
#endif
    return WM_SUCCESS;
}

static mlan_status sd_wifi_preinit(void)
{
    mlan_status mlanstatus = MLAN_STATUS_SUCCESS;

    /* initializes the driver struct */
    int sdiostatus = wlan_init_struct();
    if (sdiostatus != WM_SUCCESS)
    {
        wifi_io_e("Init failed. Cannot create init struct");
        return MLAN_STATUS_FAILURE;
    }

    /*
     * Register a callback with power manager of MC200/MW300
     * This callback will be called on entry /exit
     * of low power mode of MC200/MW300 based on first paramter
     * passed to the call.
     */
    //	pm_handle = pm_register_cb(ACTION_EXIT_PM3 | ACTION_ENTER_PM4,
    //		       sd_wifi_ps_cb, NULL);

    (void)mlan_subsys_init();
    seqnum   = 0;
    txportno = 0;
    return mlanstatus;
}

void sd_wifi_reset_ports()
{
#if defined(SD8801)
    txportno = 1;
    mlan_adap->curr_rd_port = 1;
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    txportno = 0;
    mlan_adap->curr_rd_port = 0;
#endif
}

mlan_status sd_wifi_post_init(enum wlan_type type)
{
    mlan_status mlanstatus = MLAN_STATUS_SUCCESS;
    uint32_t resp;
    bool invalid_wlantype = MFALSE;
#if defined(SD8801)
    sdio_drv_creg_write(HOST_INT_MASK_REG, 1, 0x3, &resp);
#elif defined(SD8978) || defined(SD8987) || defined(SD8997) || defined(SD9097) || defined(SD9098) || defined(SD9177)
    (void)sdio_drv_creg_write(HOST_INT_MASK_REG, 1, HIM_ENABLE, &resp);
#endif

    /* If we're running a Manufacturing image, start the tasks.
       If not, initialize and setup the firmware */
    switch (type)
    {
        case WLAN_TYPE_NORMAL:
            fw_init_cfg = 1;
            wlan_fw_init_cfg();
            fw_init_cfg = 0;
            break;
        case WLAN_TYPE_WIFI_CALIB:
            g_txrx_flag = true;
            break;
        case WLAN_TYPE_FCC_CERTIFICATION:
            g_txrx_flag = true;
            break;
        default:
            wifi_io_e("Enter a valid input to sd_wifi_init");
            invalid_wlantype = MTRUE;
            break;
    }

    if (invalid_wlantype == MTRUE)
    {
        return MLAN_STATUS_FAILURE;
    }

    sd_wifi_reset_ports();

    return mlanstatus;
}

static void *intf;

mlan_status sd_wifi_init(enum wlan_type type, const uint8_t *fw_start_addr, const size_t size)
{
    mlan_status ret = MLAN_STATUS_SUCCESS;

    ret = sd_wifi_preinit();
    if (ret == MLAN_STATUS_SUCCESS)
    {
        intf = (void *)sdio_init_interface(NULL);
        if (intf != MNULL)
        {
            ret = (mlan_status)firmware_download(fw_start_addr, size, intf, 0);
        } else {
            ret = MLAN_STATUS_FAILURE;
	}
    }
    return ret;
}

#if defined(CONFIG_WIFI_IND_DNLD)
mlan_status sd_wifi_reinit(enum wlan_type type, const uint8_t *fw_start_addr, const size_t size, uint8_t fw_reload)
{
    mlan_status ret = MLAN_STATUS_SUCCESS;

    if (fw_reload == FW_RELOAD_NO_EMULATION)
    {
        int sta = (int)os_enter_critical_section();
        /* Allow interrupt handler to deliver us a packet */
        g_txrx_flag = false;

        sdio_disable_interrupt();

        os_exit_critical_section((unsigned long)sta);

        sdio_ioport_init();
    }

    ret = (mlan_status)firmware_download(fw_start_addr, size, intf, fw_reload);

    if (ret != MLAN_STATUS_FAILURE)
    {
        if (fw_reload == FW_RELOAD_NO_EMULATION)
        {
            int sta = (int)os_enter_critical_section();
            /* Allow interrupt handler to deliver us a packet */
            g_txrx_flag = true;

            sdio_enable_interrupt();

            os_exit_critical_section((unsigned long)sta);
        }
    }

    return ret;
}
#endif


void sd_wifi_deinit(void)
{
#ifdef WLAN_LOW_POWER_ENABLE
    low_power_mode = false;
#endif
    cal_data_valid = false;
    mac_addr_valid = false;
    //	pm_deregister_cb(pm_handle);

    (void)wlan_cmd_shutdown();
    sdio_drv_deinit();
    (void)mlan_subsys_deinit();
    (void)wlan_deinit_struct();
}

#ifdef CONFIG_FW_VDLL
HostCmd_DS_COMMAND *wifi_get_vdllcommand_buffer(void)
{
    /* First 4 bytes reserved for SDIO pkt header */
    return (HostCmd_DS_COMMAND *)(void *)(vdll_cmd_buf + INTF_HEADER_LEN);
}
#endif

HostCmd_DS_COMMAND *wifi_get_command_buffer(void)
{
    /* First 4 bytes reserved for SDIO pkt header */
    return (HostCmd_DS_COMMAND *)(void *)(ctrl_cmd_buf + INTF_HEADER_LEN);
}

/**
 * Function to set mlan ioport.
 */
void set_ioport_inmlan(t_u32 port)
{
    mlan_adap->ioport = port;
}

#ifdef CONFIG_HOST_SLEEP
void wifi_print_wakeup_reason(t_u16 hs_wakeup_reason)
{
    if (hs_wakeup_reason == 0)
    {
        PRINTF("Woken up by unknown reason\r\n");
    }
    else if (hs_wakeup_reason == 1)
    {
        PRINTF("Woken up by Broadcast data matched\r\n");
    }
    else if (hs_wakeup_reason == 2)
    {
        PRINTF("Woken up by Multicast data matched\r\n");
    }
    else if (hs_wakeup_reason == 3)
    {
        PRINTF("Woken up by Unicast data matched\r\n");
    }
    else if (hs_wakeup_reason == 4)
    {
        PRINTF("Woken up by Maskable event matched\r\n");
    }
    else if (hs_wakeup_reason == 5)
    {
        PRINTF("Woken up by Non-maskable event matched\r\n");
    }
    else if (hs_wakeup_reason == 6)
    {
        PRINTF("Woken up by Non-maskable condition matched (EAPoL rekey)\r\n");
    }
    else if (hs_wakeup_reason == 7)
    {
        PRINTF("Woken up by Magic pattern matched\r\n");
    }
    else if (hs_wakeup_reason == 8)
    {
        PRINTF("Woken up by control frames matched \r\n");
    }
    else if (hs_wakeup_reason == 9)
    {
        PRINTF("Woken up by management frame matched \r\n");
    }
    else if (hs_wakeup_reason == 10)
    {
        PRINTF("Woken up by GTK rekey failure\r\n");
    }
    else if (hs_wakeup_reason == 11)
    {
        PRINTF("Woken up by management frame filter extension matched\r\n");
    }
    else
    {
        PRINTF("Woken up by reserved reason\r\n");
    }
}

void wifi_clear_wakeup_reason(void)
{
    if (mlan_adap != NULL)
    {
        memset(&mlan_adap->wlan_wakeup, 0x0, sizeof(wlan_wakeup_reason));
    }
}
#endif