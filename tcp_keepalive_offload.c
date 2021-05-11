/******************************************************************************
* File Name:   tcp_keepalive_offload.c
*
* Description: This file handles the Wi-Fi connection to the Access Point,
*              establishes TCP socket connections with the remote server,
*              and handles the low power task which suspends the network
*              stack forever.
*
* Related Document: See README.md
*
********************************************************************************
* Copyright 2020-2021, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

#include <inttypes.h>

/* LPA header file */
#include "cy_OlmInterface.h"
#include "network_activity_handler.h"

/* lwIP header file */
#include "cy_lwip.h"

/* Socket management header file */
#include "cy_secure_sockets.h"

/* Wi-Fi connection manager header file */
#include "cy_wcm.h"

/* User settings related to Wi-Fi and network stack */
#include "app_config.h"

/* Provides function prototypes for Wi-Fi connection,
 * Socket connection, and for the task that suspends
 * the network stack.
 */
#include "tcp_keepalive_offload.h"

/*******************************************************************************
* Macros
********************************************************************************/
#define TCP_KEEPALIVE_OFFLOAD             "TKO"

/*******************************************************************************
* Global Variables
********************************************************************************/
/* Connection parameters to the Wi-Fi connection manager (WCM). */
cy_wcm_connect_params_t connect_param = {0};
cy_wcm_ip_address_t ip_addr;

/* TCP socket handle for each connection */
struct cy_socket_ctx_t *global_socket[MAX_TKO] = { NULL };

/********************************************************************************
 * Function Name: find_my_tko_descriptor
 ********************************************************************************
 * Summary:
 *  Finds the OLM (Offload Manager) descriptor for the given offload type and return
 *  the offload list.
 *
 * Parameters:
 *  name: Offload type for which the configuration list is requested.
 *
 * Return:
 *  const ol_desc_t *: Reference to offload configuration for the given offload type.
 *
 *******************************************************************************/
const ol_desc_t *find_my_tko_descriptor(const char *name)
{
    const ol_desc_t *offloads_list;

    /* Take offload configuration defined by the configurator */
    offloads_list = (const ol_desc_t *)get_default_ol_list();

    /* Return the TCP keepalive offload configuration if it exists. */
    while (offloads_list && offloads_list->name &&
           (0 != strncmp(offloads_list->name, name, strlen(name))))
    {
        offloads_list++;
    }

    if (!offloads_list || !offloads_list->name)
    {
        ERR_INFO(("Unable to find %s offloads configuration\n", name));
        offloads_list = NULL;
    }

    return offloads_list;
}

/********************************************************************************
 * Function Name: network_idle_task
 ********************************************************************************
 * Summary:
 *  Suspends the lwIP network stack indefinitely which helps the RTOS to enter the
 *  Idle state, and then eventually into deep-sleep power mode. The MCU will stay in
 *  deep-sleep power mode till the network stack resumes. The network stack resumes
 *  whenever any Tx/Rx activity is detected in the EMAC interface (path between Wi-Fi
 *  driver and network stack).
 *
 * Parameters:
 *  void *arg: Task specific arguments. Never used.
 *
 * Return:
 *  void: Never returns. Forever it tries to suspend the network stack.
 *
 *******************************************************************************/
void network_idle_task(void *arg)
{
    struct netif* wifi = cy_lwip_get_interface(CY_LWIP_STA_NW_INTERFACE);

    while( true )
    {
        /* Suspend the network stack */
        wait_net_suspend(wifi,
                         portMAX_DELAY,
                         NETWORK_INACTIVE_INTERVAL_MS,
                         NETWORK_INACTIVE_WINDOW_MS);

        /*
         * Safe delay to avoid race conditions when switching between
         * offload enable and disable states when network stack is suspended
         * and resumed by the offload manager.
         */
        vTaskDelay(pdMS_TO_TICKS(NETWORK_SUSPEND_DELAY_MS));
    }
}

/********************************************************************************
 * Function Name: tcp_socket_connection_start
 ********************************************************************************
 * Summary:
 *  Establishes TCP socket connection with the TCP server. Maximum up to MAX_TKO
 *  number of connections are allowed as defined by the LPA (Low Power Assistant)
 *  middleware.
 *
 * Parameters:
 *  void
 *
 * Return:
 *  cy_rslt_t: Returns CY_RSLT_SUCCESS if the connection is successfully created
 *  for all the configured sockets, a socket error code otherwise.
 *
 *******************************************************************************/
cy_rslt_t tcp_socket_connection_start(void)
{
    const ol_desc_t *offload_list;
    const cy_tko_ol_cfg_t *downloaded;
    cy_rslt_t result = CY_RSLT_SUCCESS;
    const cy_tko_ol_connect_t *port;
    int index = 0;
    struct netif *netif = cy_lwip_get_interface(CY_LWIP_STA_NW_INTERFACE);
    cy_rslt_t socket_connection_status = CY_RSLT_SUCCESS;

    result = cy_socket_init();

    PRINT_AND_ASSERT(result, "%s Socket initialization failed. Error code:%"PRIu32"\n", __func__, result);

    /* Take reference to the configured offload list */
    offload_list = find_my_tko_descriptor(TCP_KEEPALIVE_OFFLOAD);

    if (NULL == offload_list)
    {
        ERR_INFO(("Offload configuration not found.\n"));
        return CY_RSLT_TYPE_ERROR;
    }

    /* Take reference to the TCP Keepalive configuration */
    downloaded = (const cy_tko_ol_cfg_t *)offload_list->cfg;

    if (NULL != downloaded)
    {
        APP_INFO(("Taking TCP Keepalive configuration from the Generated sources.\n"));
        /*
         * Offload descriptor was found.
         * Start TCP socket connection to the configured TCP server.
         */
        for (index = 0; index < MAX_TKO; index++)
        {
             port = &downloaded->ports[index];

             if ((port->remote_port > 0) &&
                 (port->local_port > 0) &&
                 (strcmp(port->remote_ip, NULL_IP_ADDRESS) != 0))
             {
                 /*
                  * Configure TCP Keepalive with the given remote TCP server.
                  * This is a helper function which creates a socket, binds to
                  * the socket, and then establishes TCP connection with the given
                  * TCP remote server. Enable(1) or Disable(0) the Host TCP keepalive
                  * using the macro ENABLE_HOST_TCP_KEEPALIVE.
                  */
                 result = cy_tcp_create_socket_connection(netif,
                                                          (void **)&global_socket[index],
                                                          port->remote_ip,
                                                          port->remote_port,
                                                          port->local_port,
                                                          (cy_tko_ol_cfg_t *)downloaded,
                                                          ENABLE_HOST_TCP_KEEPALIVE);

                 if (CY_RSLT_SUCCESS != result)
                 {
                     ERR_INFO(("Socket[%d]: ERROR %"PRIu32", Unable to connect. TCP Server IP: %s, Local Port: %d, "
                                                       "Remote Port: %d\n", index, result, port->remote_ip,
                                                                      port->local_port, port->remote_port));
                     socket_connection_status = result;
                 }
                 else
                 {
                     APP_INFO(("Socket[%d]: Created connection to IP %s, local port %d, remote port %d\n",
                                          index, port->remote_ip, port->local_port, port->remote_port));
                 }
             }
             else
             {
                 APP_INFO(("Skipped TCP socket connection for socket id[%d]. Check the TCP Keepalive "
                                                                        "configuration.\n", index));
             }
        }
    }
    else
    {
        ERR_INFO(("%s: Offload descriptor %s not found. No TCP connection has been established.\n"
                "Check the TCP Keepalive offload settings in ModusToolbox Device Configurator tool\n", __func__, "TKO"));
        socket_connection_status = CY_RSLT_TYPE_ERROR;
    }

    return socket_connection_status;
}

/********************************************************************************
 * Function Name: wifi_connect
 ********************************************************************************
 * Summary:
 *  The device associates to the Access Point with given SSID, PASSWORD, and SECURITY
 *  type. It retries for MAX_WIFI_RETRY_COUNT times if the Wi-Fi connection fails.
 *
 * Parameters:
 *  void
 *
 * Return:
 *  cy_rslt_t: Returns CY_RSLT_SUCCESS if the Wi-Fi connection is successfully
 *  established, a WCM error code otherwise.
 *
 *******************************************************************************/
cy_rslt_t wifi_connect(void)
{
    cy_rslt_t result = CY_RSLT_SUCCESS;
    uint32_t retry_count = 0;
    cy_wcm_config_t wcm_config = {.interface = CY_WCM_INTERFACE_TYPE_STA};

    result = cy_wcm_init(&wcm_config);

    if (CY_RSLT_SUCCESS == result)
    {
        APP_INFO(("Wi-Fi initialization is successful\n"));
        memcpy(&connect_param.ap_credentials.SSID, WIFI_SSID, sizeof(WIFI_SSID));
        memcpy(&connect_param.ap_credentials.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD));
        connect_param.ap_credentials.security = WIFI_SECURITY_TYPE;
        APP_INFO(("Join to AP: %s\n", connect_param.ap_credentials.SSID));

        /*
         * Connect to Access Point. It validates the connection parameters
         * and then establishes connection to AP.
         */
        for (retry_count = 0; retry_count < MAX_WIFI_RETRY_COUNT; retry_count++)
        {
             result = cy_wcm_connect_ap(&connect_param, &ip_addr);

             if (CY_RSLT_SUCCESS == result)
             {
                 APP_INFO(("Successfully joined wifi network %s\n", connect_param.ap_credentials.SSID));

                 if (CY_WCM_IP_VER_V4 == ip_addr.version)
                 {
                     APP_INFO(("Assigned IP address: %s\n", ip4addr_ntoa((const ip4_addr_t *)&ip_addr.ip.v4)));
                 }

                 break;
             }

             ERR_INFO(("Failed to join Wi-Fi network. Retrying...\n"));
        }
    }

    return result;
}


/* [] END OF FILE */

