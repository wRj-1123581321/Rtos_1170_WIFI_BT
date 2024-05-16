/** @file main.c
 *
 *  @brief main file
 *
 *  Copyright 2020 NXP
 *  All rights reserved.
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 */

///////////////////////////////////////////////////////////////////////////////
//  Includes
///////////////////////////////////////////////////////////////////////////////

// SDK Included Files
#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
#include "fsl_debug_console.h"
#include "wlan_bt_fw.h"
#include "wlan.h"
#include "wifi.h"
#include "wm_net.h"
#include <wm_os.h>
#include "dhcp-server.h"
#include "cli.h"
#include "wifi_ping.h"
#include "iperf.h"
#include "wifi_bt_config.h"
#include "cli_utils.h"
#ifdef CONFIG_HOST_SLEEP
#include "host_sleep.h"
#endif

#include "fsl_common.h"
/*******************************************************************************
 * Definitions
 ******************************************************************************/


/*******************************************************************************
 * Prototypes
 ******************************************************************************/
int wlan_driver_init(void);
int wlan_driver_deinit(void);
int wlan_driver_reset(void);
int wlan_reset_cli_init(void);

/*******************************************************************************
 * Code
 ******************************************************************************/

const int TASK_MAIN_PRIO = OS_PRIO_3;
const int TASK_MAIN_STACK_SIZE = 800;

portSTACK_TYPE *task_main_stack = NULL;
TaskHandle_t task_main_task_handler;

static void printSeparator(void)
{
    PRINTF("========================================\r\n");
}

static struct wlan_network sta_network;
static struct wlan_network uap_network;
int network_is_start = 0;

/* Callback Function passed to WLAN Connection Manager. The callback function
 * gets called when there are WLAN Events that need to be handled by the
 * application.
 */
int wlan_event_callback(enum wlan_event_reason reason, void *data)
{
    int ret;
    struct wlan_ip_config addr;
    char ip[16];
    static int auth_fail                      = 0;
    wlan_uap_client_disassoc_t *disassoc_resp = data;

    switch (reason)
    {
        case WLAN_REASON_INITIALIZED:
            PRINTF("app_cb: WLAN initialized\r\n");
            printSeparator();

            ret = wlan_basic_cli_init();
            if (ret != WM_SUCCESS)
            {
                PRINTF("Failed to initialize BASIC WLAN CLIs\r\n");
                return 0;
            }

            ret = wlan_cli_init();
            if (ret != WM_SUCCESS)
            {
                PRINTF("Failed to initialize WLAN CLIs\r\n");
                return 0;
            }
            PRINTF("WLAN CLIs are initialized\r\n");
            printSeparator();

            ret = wlan_enhanced_cli_init();
            if (ret != WM_SUCCESS)
            {
                PRINTF("Failed to initialize WLAN CLIs\r\n");
                return 0;
            }
            PRINTF("ENHANCED WLAN CLIs are initialized\r\n");
            printSeparator();

            ret = ping_cli_init();
            if (ret != WM_SUCCESS)
            {
                PRINTF("Failed to initialize PING CLI\r\n");
                return 0;
            }

            ret = iperf_cli_init();
            if (ret != WM_SUCCESS)
            {
                PRINTF("Failed to initialize IPERF CLI\r\n");
                return 0;
            }
            extern void gsd_task_cmd_init();
            gsd_task_cmd_init();

            ret = dhcpd_cli_init();
            if (ret != WM_SUCCESS)
            {
                PRINTF("Failed to initialize DHCP Server CLI\r\n");
                return 0;
            }

            PRINTF("CLIs Available:\r\n");
            printSeparator();
            help_command(0, NULL);
            printSeparator();
			network_is_start = 1;
            break;
        case WLAN_REASON_INITIALIZATION_FAILED:
            PRINTF("app_cb: WLAN: initialization failed\r\n");
            break;
        case WLAN_REASON_AUTH_SUCCESS:
            PRINTF("app_cb: WLAN: authenticated to network\r\n");
            break;
        case WLAN_REASON_SUCCESS:
            PRINTF("app_cb: WLAN: connected to network\r\n");
            ret = wlan_get_address(&addr);
            if (ret != WM_SUCCESS)
            {
                PRINTF("failed to get IP address\r\n");
                return 0;
            }

            net_inet_ntoa(addr.ipv4.address, ip);

            ret = wlan_get_current_network(&sta_network);
            if (ret != WM_SUCCESS)
            {
                PRINTF("Failed to get External AP network\r\n");
                return 0;
            }

            PRINTF("Connected to following BSS:\r\n");
            PRINTF("SSID = [%s]\r\n", sta_network.ssid);
            if (addr.ipv4.address != 0U)
            {
                PRINTF("IPv4 Address: [%s]\r\n", ip);
            }
#ifdef CONFIG_IPV6
            int i;
            for (i = 0; i < CONFIG_MAX_IPV6_ADDRESSES; i++)
            {
                if (ip6_addr_isvalid(addr.ipv6[i].addr_state))
                {
                    (void)PRINTF("IPv6 Address: %-13s:\t%s (%s)\r\n",
                                 ipv6_addr_type_to_desc((struct net_ipv6_config *)&addr.ipv6[i]),
                                 inet6_ntoa(addr.ipv6[i].address), ipv6_addr_state_to_desc(addr.ipv6[i].addr_state));
                }
            }
            (void)PRINTF("\r\n");
#endif
            auth_fail = 0;
            break;
        case WLAN_REASON_CONNECT_FAILED:
            PRINTF("app_cb: WLAN: connect failed\r\n");
            break;
        case WLAN_REASON_NETWORK_NOT_FOUND:
            PRINTF("app_cb: WLAN: network not found\r\n");
            break;
        case WLAN_REASON_NETWORK_AUTH_FAILED:
            PRINTF("app_cb: WLAN: network authentication failed\r\n");
            auth_fail++;
            if (auth_fail >= 3)
            {
                PRINTF("Authentication Failed. Disconnecting ... \r\n");
                wlan_disconnect();
                auth_fail = 0;
            }
            break;
        case WLAN_REASON_ADDRESS_SUCCESS:
            PRINTF("network mgr: DHCP new lease\r\n");
            break;
        case WLAN_REASON_ADDRESS_FAILED:
            PRINTF("app_cb: failed to obtain an IP address\r\n");
            break;
        case WLAN_REASON_USER_DISCONNECT:
            PRINTF("app_cb: disconnected\r\n");
            auth_fail = 0;
            break;
        case WLAN_REASON_LINK_LOST:
            PRINTF("app_cb: WLAN: link lost\r\n");
            break;
        case WLAN_REASON_CHAN_SWITCH:
            PRINTF("app_cb: WLAN: channel switch\r\n");
            break;
        case WLAN_REASON_UAP_SUCCESS:
            PRINTF("app_cb: WLAN: UAP Started\r\n");
            ret = wlan_get_current_uap_network(&uap_network);

            if (ret != WM_SUCCESS)
            {
                PRINTF("Failed to get Soft AP network\r\n");
                return 0;
            }

            printSeparator();
            PRINTF("Soft AP \"%s\" started successfully\r\n", uap_network.ssid);
            printSeparator();
            if (dhcp_server_start(net_get_uap_handle()))
                PRINTF("Error in starting dhcp server\r\n");

            PRINTF("DHCP Server started successfully\r\n");
            printSeparator();
            break;
        case WLAN_REASON_UAP_CLIENT_ASSOC:
            PRINTF("app_cb: WLAN: UAP a Client Associated\r\n");
            printSeparator();
            PRINTF("Client => ");
            print_mac((const char *)data);
            PRINTF("Associated with Soft AP\r\n");
            printSeparator();
            break;
        case WLAN_REASON_UAP_CLIENT_CONN:
            PRINTF("app_cb: WLAN: UAP a Client Connected\r\n");
            printSeparator();
            PRINTF("Client => ");
            print_mac((const char *)data);
            PRINTF("Connected with Soft AP\r\n");
            printSeparator();
            break;
        case WLAN_REASON_UAP_CLIENT_DISSOC:
            printSeparator();
            PRINTF("app_cb: WLAN: UAP a Client Dissociated:");
            PRINTF(" Client MAC => ");
            print_mac((const char *)(disassoc_resp->sta_addr));
            PRINTF(" Reason code => ");
            PRINTF("%d\r\n", disassoc_resp->reason_code);
            printSeparator();
            break;
        case WLAN_REASON_UAP_STOPPED:
            PRINTF("app_cb: WLAN: UAP Stopped\r\n");
            printSeparator();
            PRINTF("Soft AP \"%s\" stopped successfully\r\n", uap_network.ssid);
            printSeparator();

            dhcp_server_stop();

            PRINTF("DHCP Server stopped successfully\r\n");
            printSeparator();
            break;
        case WLAN_REASON_PS_ENTER:
            break;
        case WLAN_REASON_PS_EXIT:
            break;
#ifdef CONFIG_WIFI_IND_DNLD
        case WLAN_REASON_FW_HANG:
        case WLAN_REASON_FW_RESET:
            break;
#endif
        default:
            PRINTF("app_cb: WLAN: Unknown Event: %d\r\n", reason);
    }
    return 0;
}

int wlan_driver_init(void)
{
    int result = 0;

    /* Initialize WIFI Driver */
    result = wlan_init(wlan_fw_bin, wlan_fw_bin_len);

    assert(0 == result);

    result = wlan_start(wlan_event_callback);

    assert(0 == result);

    return result;
}

int wlan_driver_deinit(void)
{
    int result = 0;

    result = wlan_stop();
    assert(0 == result);
    wlan_deinit(0);

    return result;
}

static void wlan_hw_reset(void)
{
    BOARD_WIFI_BT_Enable(false);
    os_thread_sleep(os_msec_to_ticks(1));
    BOARD_WIFI_BT_Enable(true);
}

int wlan_driver_reset(void)
{
    int result = 0;

    result = wlan_driver_deinit();
    assert(0 == result);

    wlan_hw_reset();

    result = wlan_driver_init();
    assert(0 == result);

    return result;
}

static void test_wlan_reset(int argc, char **argv)
{
    (void)wlan_driver_reset();
}

#ifdef CONFIG_HOST_SLEEP
static void test_mcu_suspend(int argc, char **argv)
{
    (void)mcu_suspend();
}
#endif

static struct cli_command reset_commands[] = {
    {"wlan-reset", NULL, test_wlan_reset},
#ifdef CONFIG_HOST_SLEEP
    {"mcu-suspend", NULL, test_mcu_suspend},
#endif
};

int wlan_reset_cli_init(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(reset_commands) / sizeof(struct cli_command); i++)
    {
        if (cli_register_command(&reset_commands[i]) != 0)
        {
            return -1;
        }
    }

    return 0;
}

int start_network(void)
{
	struct wlan_network network;
	int ret = 0;
    (void)memset(&network, 0, sizeof(struct wlan_network));

    //os_thread_sleep(os_msec_to_ticks(5000));
	strcpy(network.name, "add");
	strcpy(network.ssid, "NXP_SOFT");
	network.channel = 161;
	network.ip.ipv4.address = net_inet_aton("192.168.1.1");
	network.ip.ipv4.gw = net_inet_aton("192.168.1.1");
	network.ip.ipv4.netmask = net_inet_aton("255.255.255.0");
	network.security.type = WLAN_SECURITY_WPA2;
	network.security.key_mgmt |= WLAN_KEY_MGMT_PSK;
	network.security.psk_len = 8;
	strcpy(network.security.psk, "12345678");
	network.role = WLAN_BSS_ROLE_UAP;

	network.ip.ipv4.addr_type         = ADDR_TYPE_STATIC;
    network.ssid[IEEEtypes_SSID_SIZE] = '\0';
	ret                               = wlan_add_network(&network);
    switch (ret)
    {
        case WM_SUCCESS:
            (void)PRINTF("Added \"%s\"\r\n", network.name);
            break;
        case -WM_E_INVAL:
            (void)PRINTF("Error: network already exists or invalid arguments\r\n");
            break;
        case -WM_E_NOMEM:
            (void)PRINTF("Error: network list is full\r\n");
            break;
        case WLAN_ERROR_STATE:
            (void)PRINTF("Error: can't add networks in this state\r\n");
            break;
        default:
            (void)PRINTF(
                "Error: unable to add network for unknown"
                " reason\r\n");
            break;
    }
    return ret;
}
void task_main(void *param)
{
    int32_t result = 0;
	int ret = 0;
    (void)result;

    PRINTF("Initialize CLI\r\n");
    printSeparator();

    result = cli_init();

    assert(WM_SUCCESS == result);

    result = wlan_reset_cli_init();

    assert(WM_SUCCESS == result);

#ifdef CONFIG_HOST_SLEEP
    hostsleep_init(wlan_hs_pre_cfg, wlan_hs_post_cfg);
#endif


    PRINTF("Initialize WLAN Driver\r\n");
    printSeparator();

    /* Initialize WIFI Driver */
    result = wlan_driver_init();

    assert(WM_SUCCESS == result);

    while (1)
    {
        /* wait for interface up */
        os_thread_sleep(os_msec_to_ticks(5000));
		if (network_is_start == 1) {
			ret = start_network();
			//os_thread_sleep(os_msec_to_ticks(5000));
			if (ret == WM_SUCCESS) {
				ret = wlan_uap_set_bandwidth(3);
				//os_thread_sleep(os_msec_to_ticks(5000));
				if (ret == WM_SUCCESS) {
					ret = wlan_start_network("add");
				}
			}
			network_is_start = 2;
			int cpu_test();
			os_thread_sleep(os_msec_to_ticks(10000));
			//cpu_test();
		}
		
    }
}

/*******************************************************************************
 * Prototypes
 ******************************************************************************/

int main(void)
{
    BaseType_t result = 0;
    (void)result;

    BOARD_ConfigMPU();
    BOARD_InitBootPins();
    BOARD_InitPinsM2();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();

    printSeparator();
    PRINTF("wifi cli demo\r\n");
    printSeparator();

    result =
        xTaskCreate(task_main, "main", TASK_MAIN_STACK_SIZE, task_main_stack, TASK_MAIN_PRIO, &task_main_task_handler);
    assert(pdPASS == result);

    vTaskStartScheduler();
    for (;;)
        ;
}
