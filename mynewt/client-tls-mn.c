/* client-tls-mn.c
 *
 * Copyright (C) 2006-2020 wolfSSL Inc.
 *
 * This file is part of wolfSSL. (formerly known as CyaSSL)
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "os/mynewt.h"
#include <bsp/bsp.h>

#include <hal/hal_gpio.h>
#include <hal/hal_flash.h>
#include <console/console.h>

#include <log/log.h>
#include <config/config.h>
#include <hal/hal_system.h>

#include <bootutil/image.h>
#include <bootutil/bootutil.h>

#include <shell/shell.h>
#include <mn_socket/mn_socket.h>
// #include <inet_def_service/inet_def_service.h>

#include <assert.h>
#include <string.h>
#include <id/id.h>

#if MYNEWT_VAL(BUILD_WITH_OIC)
#include <oic/oc_api.h>
#include <oic/oc_log.h>
#include <cborattr/cborattr.h>
#endif

#ifdef ARCH_sim
#include <mcu/mcu_sim.h>
extern time_t time(time_t*);
#endif

/* wolfSSL */
#include <wolfssl/ssl.h>
#define USE_CERT_BUFFERS_2048
#include <wolfssl/certs_test.h>

#define DEFAULT_IPADDR "93.184.216.34" // www.example.com
#define DEFAULT_PORT 443

struct os_sem test_sem;

static struct log my_log;

#define MAX_CBMEM_BUF 2048

static uint32_t cbmem_buf[MAX_CBMEM_BUF];
static struct cbmem cbmem;

/* time command */
static int time_cli(int argc, char **argv);
struct shell_cmd time_cmd = {
    .sc_cmd = "time",
    .sc_cmd_func = time_cli,
};
static int
time_cli(int argc, char **argv)
{
    long long time;
    char *eptr;
    struct os_timeval utctime;
    struct os_timezone tz;

    if(argc < 2) {
        return 0;
    }
    time = strtoul(argv[1], &eptr, 0);
    if (*eptr != '\0') {
        console_printf("Invalid time %s\n", argv[3]);
        return 0;
    }

    utctime.tv_sec = time;
    utctime.tv_usec = 0;
    tz.tz_minuteswest = 0;
    tz.tz_dsttime = 0;
    os_settimeofday(&utctime, &tz);

    return 0;
}

/* net command */
static int net_cli(int argc, char **argv);
struct shell_cmd net_test_cmd = {
    .sc_cmd = "net",
    .sc_cmd_func = net_cli
};

static struct mn_socket *net_test_socket;
static struct mn_socket *net_test_socket2;
static struct mn_sockaddr_in net_test_sin;

#if MYNEWT_VAL(BUILD_WITH_OIC)
static void omgr_app_init(void);
static const oc_handler_t omgr_oc_handler = {
    .init = omgr_app_init,
};
#endif

static void net_test_readable(void *arg, int err)
{
    console_printf("net_test_readable %x - %d\n", (int)arg, err);
}

static void net_test_writable(void *arg, int err)
{
    console_printf("net_test_writable %x - %d\n", (int)arg, err);
}

static const union mn_socket_cb net_test_cbs = {
    .socket.readable = net_test_readable,
    .socket.writable = net_test_writable
};

static int net_test_newconn(void *arg, struct mn_socket *new)
{
    console_printf("net_test_newconn %x - %x\n", (int)arg, (int)new);
    mn_socket_set_cbs(new, NULL, &net_test_cbs);
    net_test_socket2 = new;
    return 0;
}

static const union mn_socket_cb net_listen_cbs =  {
    .listen.newconn = net_test_newconn,
};

static int
net_cli(int argc, char **argv)
{
    int rc;
    struct mn_sockaddr_in sin;
    struct mn_sockaddr_in *sinp;
    uint16_t port;
    uint32_t addr;
    char *eptr;
    struct os_mbuf *m;

    if (argc < 2) {
        return 0;
    }
    if (!strcmp(argv[1], "udp")) {
        rc = mn_socket(&net_test_socket, MN_PF_INET, MN_SOCK_DGRAM, 0);
        console_printf("mn_socket(UDP) = %d %x\n", rc,
          (int)net_test_socket);
    } else if (!strcmp(argv[1], "tcp")) {
        rc = mn_socket(&net_test_socket, MN_PF_INET, MN_SOCK_STREAM, 0);
        console_printf("mn_socket(TCP) = %d %x\n", rc,
          (int)net_test_socket);
    } else if (!strcmp(argv[1], "connect") || !strcmp(argv[1], "bind")) {
        char *addrStr = DEFAULT_IPADDR;
        int port = DEFAULT_PORT;

        if(argc > 3) {
            // get ip address from argument
            addrStr = argv[2];
        }
        if(argc > 4) {
            // get port number from argument
            char *eptr = NULL;
            port = strtoul(argv[3], &eptr, 0);
            if (*eptr != '\0') {
                console_printf("Invalid port %s\n", argv[3]);
                return 0;
            }
        }
        if (mn_inet_pton(MN_AF_INET, addrStr, &addr) != 1) {
            console_printf("Invalid address %s\n", argv[2]);
            return 0;
        }

        uint8_t *ip = (uint8_t *)&addr;

        console_printf("%d.%d.%d.%d/%d\n", ip[0], ip[1], ip[2], ip[3], port);
        memset(&net_test_sin, 0, sizeof(net_test_sin));
        net_test_sin.msin_len = sizeof(net_test_sin);
        net_test_sin.msin_family = MN_AF_INET;
        net_test_sin.msin_port = htons(port);
        net_test_sin.msin_addr.s_addr = addr;

        if (!strcmp(argv[1], "connect")) {
            mn_socket_set_cbs(net_test_socket, NULL, &net_test_cbs);
            rc = mn_connect(net_test_socket, (struct mn_sockaddr *)&net_test_sin);
            console_printf("mn_connect() = %d\n", rc);
        } else {
            mn_socket_set_cbs(net_test_socket, NULL, &net_test_cbs);
            rc = mn_bind(net_test_socket, (struct mn_sockaddr *)&net_test_sin);
            console_printf("mn_bind() = %d\n", rc);
        }
    } else if (!strcmp(argv[1], "listen")) {
            mn_socket_set_cbs(net_test_socket, NULL, &net_listen_cbs);
        rc = mn_listen(net_test_socket, 2);
        console_printf("mn_listen() = %d\n", rc);
    } else if (!strcmp(argv[1], "close")) {
        rc = mn_close(net_test_socket);
        console_printf("mn_close() = %d\n", rc);
        net_test_socket = NULL;
        if (net_test_socket2) {
            rc = mn_close(net_test_socket2);
            console_printf("mn_close() = %d\n", rc);
            net_test_socket2 = NULL;
        }
    } else if (!strcmp(argv[1], "send")) {
        if (argc < 3) {
            return 0;
        }
        m = os_msys_get_pkthdr(16, 0);
        if (!m) {
            console_printf("out of mbufs\n");
            return 0;
        }
        rc = os_mbuf_copyinto(m, 0, argv[2], strlen(argv[2]));
        if (rc < 0) {
            console_printf("can't copy data\n");
            os_mbuf_free_chain(m);
            return 0;
        }
        if (argc > 4) {
            if (mn_inet_pton(MN_AF_INET, argv[3], &addr) != 1) {
                console_printf("Invalid address %s\n", argv[2]);
                return 0;
            }

            port = strtoul(argv[4], &eptr, 0);
            if (*eptr != '\0') {
                console_printf("Invalid port %s\n", argv[3]);
                return 0;
            }
            uint8_t *ip = (uint8_t *)&addr;

            console_printf("%d.%d.%d.%d/%d\n", ip[0], ip[1], ip[2], ip[3],
              port);
            memset(&sin, 0, sizeof(sin));
            sin.msin_len = sizeof(sin);
            sin.msin_family = MN_AF_INET;
            sin.msin_port = htons(port);
            sin.msin_addr.s_addr = addr;
            sinp = &sin;
        } else {
            sinp = NULL;
        }

        if (net_test_socket2) {
            rc = mn_sendto(net_test_socket2, m, (struct mn_sockaddr *)sinp);
        } else {
            rc = mn_sendto(net_test_socket, m, (struct mn_sockaddr *)sinp);
        }
        console_printf("mn_sendto() = %d\n", rc);
    } else if (!strcmp(argv[1], "peer")) {
        if (net_test_socket2) {
            rc = mn_getpeername(net_test_socket2, (struct mn_sockaddr *)&sin);
        } else {
            rc = mn_getpeername(net_test_socket, (struct mn_sockaddr *)&sin);
        }
        console_printf("mn_getpeername() = %d\n", rc);
        uint8_t *ip = (uint8_t *)&sin.msin_addr;

        console_printf("%d.%d.%d.%d/%d\n", ip[0], ip[1], ip[2], ip[3],
          ntohs(sin.msin_port));
    } else if (!strcmp(argv[1], "recv")) {
        if (net_test_socket2) {
            rc = mn_recvfrom(net_test_socket2, &m, (struct mn_sockaddr *)&sin);
        } else {
            rc = mn_recvfrom(net_test_socket, &m, (struct mn_sockaddr *)&sin);
        }
        console_printf("mn_recvfrom() = %d\n", rc);
        if (m) {
            uint8_t *ip = (uint8_t *)&sin.msin_addr;

            console_printf("%d.%d.%d.%d/%d\n", ip[0], ip[1], ip[2], ip[3],
              ntohs(sin.msin_port));
            m->om_data[m->om_len] = '\0';
            console_printf("received %d bytes >%s<\n",
              OS_MBUF_PKTHDR(m)->omp_len, (char *)m->om_data);
            os_mbuf_free_chain(m);
        }
    } else if (!strcmp(argv[1], "mcast_join") ||
      !strcmp(argv[1], "mcast_leave")) {
        struct mn_mreq mm;
        int val;

        if (argc < 4) {
            return 0;
        }

        val = strtoul(argv[2], &eptr, 0);
        if (*eptr != '\0') {
            console_printf("Invalid itf_idx %s\n", argv[2]);
            return 0;
        }

        memset(&mm, 0, sizeof(mm));
        mm.mm_idx = val;
        mm.mm_family = MN_AF_INET;
        if (mn_inet_pton(MN_AF_INET, argv[3], &mm.mm_addr) != 1) {
            console_printf("Invalid address %s\n", argv[2]);
            return 0;
        }
        if (!strcmp(argv[1], "mcast_join")) {
            val = MN_MCAST_JOIN_GROUP;
        } else {
            val = MN_MCAST_LEAVE_GROUP;
        }
        rc = mn_setsockopt(net_test_socket, MN_SO_LEVEL, val, &mm);
        console_printf("mn_setsockopt() = %d\n", rc);
    } else if (!strcmp(argv[1], "listif")) {
        struct mn_itf itf;
        struct mn_itf_addr itf_addr;
        char addr_str[48];

        memset(&itf, 0, sizeof(itf));
        while (1) {
            rc = mn_itf_getnext(&itf);
            if (rc) {
                break;
            }
            console_printf("%d: %x %s\n", itf.mif_idx, itf.mif_flags,
              itf.mif_name);
            memset(&itf_addr, 0, sizeof(itf_addr));
            while (1) {
                rc = mn_itf_addr_getnext(&itf, &itf_addr);
                if (rc) {
                    break;
                }
                mn_inet_ntop(itf_addr.mifa_family, &itf_addr.mifa_addr,
                  addr_str, sizeof(addr_str));
                console_printf(" %s/%d\n", addr_str, itf_addr.mifa_plen);
            }
        }
#if MYNEWT_VAL(MCU_STM32F4) || MYNEWT_VAL(MCU_STM32F7)
    } else if (!strcmp(argv[1], "mii")) {
        extern int stm32_mii_dump(int (*func)(const char *fmt, ...));

        stm32_mii_dump(console_printf);
#endif
    /* 
    } else if (!strcmp(argv[1], "service")) {
        inet_def_service_init(os_eventq_dflt_get());
    */
#if MYNEWT_VAL(BUILD_WITH_OIC)
    } else if (!strcmp(argv[1], "oic")) {
        oc_main_init((oc_handler_t *)&omgr_oc_handler);
#endif
    } else {
        console_printf("unknown cmd\n");
    }
    return 0;
}

/* wolfssl command */
static int wolfssl_cli(int argc, char **argv);
struct shell_cmd wolfssl_cmd = {
    .sc_cmd = "wolfssl",
    .sc_cmd_func = wolfssl_cli,
};

static WOLFSSL_CTX* wolfsslCtx = NULL;
static WOLFSSL*     ssl = NULL;

static int wolfssl_ctx_init() {
    if(wolfsslCtx && ssl) {
        console_printf("ERROR: already initialize WOLFSSL_CTX and ssl\n");
        return -1; // already init
    }

    /* Create and initialize WOLFSSL_CTX */
    if ((wolfsslCtx = wolfSSL_CTX_new(wolfTLSv1_2_client_method())) == NULL) {
        console_printf("ERROR: failed to create WOLFSSL_CTX\n");
        return -1;
    }

    int ret = wolfSSL_CTX_load_verify_buffer(wolfsslCtx, ca_cert_der_2048, sizeof_ca_cert_der_2048, SSL_FILETYPE_ASN1);
    if(ret != SSL_SUCCESS) {
        console_printf("Error %d loading CA cert\n", ret);
        return ret;
    }

    wolfSSL_CTX_set_verify(wolfsslCtx, SSL_VERIFY_NONE, 0);
    /* Create a WOLFSSL object */
    if ((ssl = wolfSSL_new(wolfsslCtx)) == NULL) {
        console_printf("ERROR: failed to create WOLFSSL object\n");
        return -1;
    }

    console_printf("wolfssl contexts are initialized\n");
    return 0;
}
static void wolfssl_ctx_clear() {
    if(ssl) {
        wolfSSL_free(ssl);             /* Free the wolfSSL object                  */
        ssl = NULL;
    }
    if(wolfsslCtx) {
        wolfSSL_CTX_free(wolfsslCtx);  /* Free the wolfSSL context object          */
        wolfsslCtx = NULL;
    }
    console_printf("clear wolfssl contexts\n");
}
static int
wolfssl_cli(int argc, char **argv)
{
    int rc;
    int err = 0;

    if (argc < 2) {
        return 0;
    }
    const char *subCommand = argv[1];

    if(!strcmp(subCommand, "init")) {
        wolfssl_ctx_init();
        wolfSSL_SetIO_Mynewt(ssl, net_test_socket, &net_test_sin);
        console_printf("wolfSSL ctx initialize\n");
    } else if(!strcmp(subCommand, "clear")) {
        wolfssl_ctx_clear();
        console_printf("wolfSSL ctx clear\n");
    } else if(!strcmp(subCommand, "connect")) {
        rc = wolfSSL_connect(ssl);
        if(rc != SSL_SUCCESS) {
            err = wolfSSL_get_error(ssl, 0);
            console_printf("ERROR: wolfSSL_connect rc:%d err:%d\n", rc, err);
            return 0;
        }
        console_printf("wolfSSL_connect() = %d\n", rc);
    } else if(!strcmp(subCommand, "write")) {
        char *str = "GET /index.html HTTP/1.0\r\n\r\n";
        int len;
        if(argc > 3) {
            str = argv[2];
        }
        len = strlen(str);

        rc = wolfSSL_write(ssl, str, len);
        if(rc != len) {
            err = wolfSSL_get_error(ssl, 0);
            console_printf("ERROR: wolfSSL_write rc:%d err:%d\n", rc, err);
            return -1;
        }
        console_printf("wolfSSL_write() = %dL\n", rc);
    } else if(!strcmp(subCommand, "read")) {
        char buff[256];
        rc = 0;
        while(rc >= 0) {
            memset(buff, 0, sizeof(buff));
            rc = wolfSSL_read(ssl, buff, sizeof(buff)-1);
            if(rc < 0) {
                err = wolfSSL_get_error(ssl, 0);
                console_printf("ERROR: wolfSSL_read rc:%d err:%d\n", rc, err);
                return 0;
            }
            console_printf("%.*s\n", rc, buff);
        }
        console_printf("\n");

    } else {
        console_printf("ERROR: unknown command: %s\n", subCommand);
    }
    
    return 0;
}


#if MYNEWT_VAL(BUILD_WITH_OIC)
static void
app_get_light(oc_request_t *request, oc_interface_mask_t interface)
{
    bool value;

    if (hal_gpio_read(LED_BLINK_PIN)) {
        value = true;
    } else {
        value = false;
    }
    oc_rep_start_root_object();
    switch (interface) {
    case OC_IF_BASELINE:
        oc_process_baseline_interface(request->resource);
    case OC_IF_A:
        oc_rep_set_boolean(root, value, value);
        break;
    default:
        break;
    }
    oc_rep_end_root_object();
    oc_send_response(request, OC_STATUS_OK);
}

static void
app_set_light(oc_request_t *request, oc_interface_mask_t interface)
{
    bool value;
    int len;
    uint16_t data_off;
    struct os_mbuf *m;
    struct cbor_attr_t attrs[] = {
        [0] = {
            .attribute = "value",
            .type = CborAttrBooleanType,
            .addr.boolean = &value,
            .dflt.boolean = false
        },
        [1] = {
        }
    };

    len = coap_get_payload(request->packet, &m, &data_off);
    if (cbor_read_mbuf_attrs(m, data_off, len, attrs)) {
        oc_send_response(request, OC_STATUS_BAD_REQUEST);
    } else {
        hal_gpio_write(LED_BLINK_PIN, value == true);
        oc_send_response(request, OC_STATUS_CHANGED);
    }
}

static void
omgr_app_init(void)
{
    oc_resource_t *res;

    oc_init_platform("MyNewt", NULL, NULL);
    oc_add_device("/oic/d", "oic.d.light", "MynewtLed", "1.0", "1.0", NULL,
                  NULL);

    res = oc_new_resource("/light/1", 1, 0);
    oc_resource_bind_resource_type(res, "oic.r.switch.binary");
    oc_resource_bind_resource_interface(res, OC_IF_A);
    oc_resource_set_default_interface(res, OC_IF_A);

    oc_resource_set_discoverable(res);
    oc_resource_set_periodic_observable(res, 1);
    oc_resource_set_request_handler(res, OC_GET, app_get_light);
    oc_resource_set_request_handler(res, OC_PUT, app_set_light);
    oc_resource_set_request_handler(res, OC_POST, app_set_light);
    oc_add_resource(res);

    hal_gpio_init_out(LED_BLINK_PIN, 1);
}
#endif

/**
 * main
 *
 * The main task for the project. This function initializes the packages, calls
 * init_tasks to initialize additional tasks (and possibly other objects),
 * then starts serving events from default event queue.
 *
 * @return int NOTE: this function should never return!
 */
int
main(int argc, char **argv)
{
#ifdef ARCH_sim
    mcu_sim_parse_args(argc, argv);
#endif

#ifndef ARCH_sim
    {
        /*
         * XXX set mac address when using STM32 ethernet XXX
         * XXX move this somewhere else XXX
         */
        static uint8_t mac[6]= { 0, 1, 1, 2, 2, 3 };
        int stm32_eth_set_hwaddr(uint8_t *addr);
        stm32_eth_set_hwaddr(mac);
    }
#endif

    sysinit();

    console_printf("client-tls-mn\n");

    cbmem_init(&cbmem, cbmem_buf, MAX_CBMEM_BUF);
    log_register("log", &my_log, &log_cbmem_handler, &cbmem, LOG_SYSLEVEL);

    /* Initialize wolfSSL */
    wolfSSL_Init();
    /* wolfSSL_Debugging_ON(); */

    shell_cmd_register(&net_test_cmd);
    shell_cmd_register(&time_cmd);
    shell_cmd_register(&wolfssl_cmd);

#ifdef ARCH_sim
    {
        time_t t = time(NULL);

        /* set dummy wallclock time. */	
        struct os_timeval utctime;	
        struct os_timezone tz;	
        utctime.tv_sec = t;
        utctime.tv_usec = 0;	
        tz.tz_minuteswest = 0;	
        tz.tz_dsttime = 0;	
        os_settimeofday(&utctime, &tz);
    }
#endif

    /*
     * As the last thing, process events from default event queue.
     */
    while (1) {
        os_eventq_run(os_eventq_dflt_get());
    }
    wolfssl_ctx_clear();
    wolfSSL_Cleanup();
}
