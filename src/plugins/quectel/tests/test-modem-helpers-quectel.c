/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2020 Aleksander Morgado <aleksander@aleksander.es>
 * Copyright (C) 2024 JUCR GmbH
 */

#include <glib.h>
#include <glib-object.h>
#include <locale.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>
#include <math.h>

#include "mm-log-test.h"
#include "mm-modem-helpers.h"
#include "mm-modem-helpers-quectel.h"

/*****************************************************************************/
/* Test ^CTZU test responses */

typedef struct {
    const gchar *response;
    gboolean     expect_supports_disable;
    gboolean     expect_supports_enable;
    gboolean     expect_supports_enable_update_rtc;
    gboolean     expect_error;
} TestCtzuResponse;

static const TestCtzuResponse test_ctzu_response[] = {
    { "+CTZU: ",        FALSE, FALSE, FALSE, TRUE  },
    { "+CTZU: ()",      FALSE, FALSE, FALSE, TRUE  },
    { "+CTZU: (,)",     FALSE, FALSE, FALSE, TRUE  },
    { "+CTZU: (0,1)",   TRUE,  TRUE,  FALSE, FALSE },
    { "+CTZU: (0,1,3)", TRUE,  TRUE,  TRUE,  FALSE },
};

static void
common_test_ctzu (const gchar *response,
                  gboolean     expect_supports_disable,
                  gboolean     expect_supports_enable,
                  gboolean     expect_supports_enable_update_rtc,
                  gboolean     expect_error)
{
    g_autoptr(GError) error = NULL;
    gboolean          res;
    gboolean          supports_disable = FALSE;
    gboolean          supports_enable = FALSE;
    gboolean          supports_enable_update_rtc = FALSE;

    res = mm_quectel_parse_ctzu_test_response (response,
                                               NULL,
                                               &supports_disable,
                                               &supports_enable,
                                               &supports_enable_update_rtc,
                                               &error);
    if (expect_error) {
        g_assert (error);
        g_assert (!res);
    } else {
        g_assert_no_error (error);
        g_assert (res);

        g_assert_cmpuint (expect_supports_disable,           ==, supports_disable);
        g_assert_cmpuint (expect_supports_enable,            ==, supports_enable);
        g_assert_cmpuint (expect_supports_enable_update_rtc, ==, supports_enable_update_rtc);
    }
}

static void
test_ctzu (void)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS (test_ctzu_response); i++)
        common_test_ctzu (test_ctzu_response[i].response,
                          test_ctzu_response[i].expect_supports_disable,
                          test_ctzu_response[i].expect_supports_enable,
                          test_ctzu_response[i].expect_supports_enable_update_rtc,
                          test_ctzu_response[i].expect_error);
}

/*****************************************************************************/
/* Test ^FIRMVERSION test responses */
static void
test_firmversion (void)
{
    gboolean valid = TRUE;

    valid = mm_quectel_check_standard_firmware_version_valid ("EM05GFAR07A07M1G_01.016.01.016");
    g_assert_cmpuint (valid, ==, TRUE);

    valid = mm_quectel_check_standard_firmware_version_valid ("EM05GFAR07A07M1G_01.016.00.000");
    g_assert_cmpuint (valid, ==, FALSE);
}

static void
test_parse_revision (void)
{
    gboolean valid;
    guint release;
    guint minor;

    valid = mm_quectel_get_version_from_revision ("EM05GFAR07A07M1G_01.016.01.016", &release, &minor, NULL);
    g_assert_cmpuint (valid, ==, TRUE);
    g_assert_cmpuint (release, ==, 7);
    g_assert_cmpuint (minor, ==, 7);

    valid = mm_quectel_get_version_from_revision ("EM05GFAR10A02M1G", &release, &minor, NULL);
    g_assert_cmpuint (valid, ==, TRUE);
    g_assert_cmpuint (release, ==, 10);
    g_assert_cmpuint (minor, ==, 2);

    valid = mm_quectel_get_version_from_revision ("EM05GFAR07AM1G", &release, &minor, NULL);
    g_assert_cmpuint (valid, ==, FALSE);

    valid = mm_quectel_get_version_from_revision ("EM05GFARA07M1G", &release, &minor, NULL);
    g_assert_cmpuint (valid, ==, FALSE);
}

/*****************************************************************************/
/* Test +QCFG test responses */

static void
test_qcfg (void)
{
    const gchar *qcfg_response =
        "+QCFG: \"urc/ri/ring\",(\"off\",\"pulse\"),(1-2000),(1-5)\r\n"
        "+QCFG: \"urc/ri/smsincoming\",(\"off\",\"pulse\"),(1-2000),(1-5)\r\n"
        "+QCFG: \"urc/ri/other\",(\"off\",\"pulse\"),(1-2000),(1-5)\r\n"
        "+QCFG: \"psm/urc\",(0,1)\r\n"
        "+QCFG: \"cmux/urcport\",(0-4)\r\n"
        "+QCFG: \"urc/delay\",(0-120)\r\n"
        "+QCFG: \"urc/cache\",(0,1)\r\n"
        "+QCFG: \"urc/port\",(\"usbat\",\"usbmodem\",\"uart1\",\"all\")\r\n"
        "+QCFG: \"risignaltype\",(\"respective\",\"physical\")\r\n"
        "+QCFG: \"usbifc\",(0,2),(0,2)\r\n"
        "+QCFG: \"roamservice\",(1,2),(0,1)\r\n"
        "+QCFG: \"nwscanmode\",(0,3)\r\n"
        "+QCFG: \"nwscanseq\",(0,3)\r\n"
        "+QCFG: \"nwscanmodeex\",(16)\r\n"
        "+QCFG: \"band\",0,(0-7FFFFFFFFFFFFFFF)\r\n"
        "+QCFG: \"nwoptmz/acq\",(0,1),(60-16777200)\r\n"
        "+QCFG: \"usbnet\",(1,3)\r\n"
        "+QCFG: \"modemrstlevel\",(0,1)\r\n"
        "+QCFG: \"aprstlevel\",(0,1)\r\n"
        "+QCFG: \"ntp\",(1-10),(5-60)\r\n"
        "+QCFG: \"ledmode\",(0-2)\r\n"
        "+QCFG: \"nat\",(0,1)\r\n"
        "+QCFG: \"netmaskset\",(0,1),<netmask>\r\n"
        "+QCFG: \"ppp/termframe\",(0,1)\r\n"
        "+QCFG: \"rf/tuner_cfg\",<index>,<let bands>\r\n"
        "+QCFG: \"tcp/windowsize\",(0,1),(16-100)\r\n"
        "+QCFG: \"TCP/SendMode\",(0-2)\r\n"
        "+QCFG: \"fast/poweroff\",(0,1)\r\n"
        "+QCFG: \"qcautoconnect\",(0,1)\r\n"
        "+QCFG: \"fota/cid\",(1-15)\r\n"
        "+QCFG: \"sms/listmsgmap\",(\"rec unread\",\"rec read\",\"sto unsend\",\"sto sent\")\r\n";
    g_autoptr(GHashTable) results;
    g_autoptr(GError)     error = NULL;

    results = mm_quectel_parse_qcfg_test_response (qcfg_response, &error);
    g_assert_no_error (error);
    g_assert_cmpint (g_hash_table_size (results), ==, 31);
    g_assert_cmpstr (g_hash_table_lookup (results, "modemrstlevel"), ==, "(0,1)");
}

/*****************************************************************************/
/* Test +QCFG usbnet response */

typedef struct {
    const gchar *response;
    gboolean     expect_success;
    const char  *expect_error;
} TestQcfgUsbnet;

static const TestQcfgUsbnet test_usbnet[] = {
    { "+QCFG: \"usbnet\",(1,3)\r\n", TRUE, NULL },
    { "+QCFG: \"usbnet\",(3)\r\n", TRUE, NULL },
    { "+QCFG: \"usbnet\",(1-3)\r\n", TRUE, NULL },
    { "+QCFG: \"usbnet\",(11,13)\r\n", FALSE, NULL },
    { "+QCFG: \"usbnet\",(0)\r\n", FALSE, NULL },
    { "+QCFG: \"adfadsfadsf\",(1,4)\r\n", FALSE, NULL },
    { "+QCFG: \"usbnet\",kdkdkdkdkkd\r\n", FALSE, NULL },
};

static void
test_qcfg_usbnet (void)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS (test_usbnet); i++) {
        g_autoptr(GHashTable) results;
        g_autoptr(GError)     error = NULL;
        gboolean              have_usbnet;

        results = mm_quectel_parse_qcfg_test_response (test_usbnet[i].response, &error);
        g_assert (results);
        g_assert_no_error (error);

        have_usbnet = mm_quectel_parse_qcfg_usbnet_support (results, &error);
        g_assert_cmpint (have_usbnet, ==, test_usbnet[i].expect_success);
        if (test_usbnet[i].expect_error) {
            g_assert_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED);
            g_assert_cmpstr (error->message, ==, test_usbnet[i].expect_error);
        } else {
            g_assert_no_error (error);
        }
    }
}

/*****************************************************************************/

typedef struct {
    const gchar              *response;
    gboolean                  expect_error;
    MMQNetdevStatusCallState  expected_v4_state;
    MMQNetdevStatusCallState  expected_v6_state;
} TestQNetdevStatusResponse;

static const TestQNetdevStatusResponse test_netdevstatus[] = {
    { "+QNETDEVSTATUS: 1,2,4,1\r\n+QNETDEVSTATUS: 1,2,6,1\r\n", FALSE, MM_QNETDEVSTATUS_CALL_STATE_CONNECTED,    MM_QNETDEVSTATUS_CALL_STATE_CONNECTED },
    { "+QNETDEVSTATUS: 1,2,4,1\r\n",                            FALSE, MM_QNETDEVSTATUS_CALL_STATE_CONNECTED,    MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED },
    { "+QNETDEVSTATUS: 1,2,6,1\r\n",                            FALSE, MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED, MM_QNETDEVSTATUS_CALL_STATE_CONNECTED },
    { "+QNETDEVSTATUS: adsfasdfasdf\r\n",                       TRUE,  MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED, MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED },
    { "+QNETDEVSTATUS: 1,7,4,1\r\n",                            TRUE,  MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED, MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED },
    { "+QNETDEVSTATUS: 1,11,4,1\r\n",                           TRUE,  MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED, MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED },
    { "+QNETDEVSTATUS: 1,2,8,1\r\n",                            TRUE,  MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED, MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED },
};

static void
test_mm_quectel_parse_qnetdevstatus_response (void)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS (test_netdevstatus); i++) {
        MMQNetdevStatusCallState  v4_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;
        MMQNetdevStatusCallState  v6_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;
        g_autoptr(GError)         error = NULL;

        if (!mm_quectel_parse_qnetdevstatus_response (test_netdevstatus[i].response,
                                                      &v4_state,
                                                      &v6_state,
                                                      &error)) {
            g_assert_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED);
        } else {
            g_assert_cmpint (v4_state, ==, test_netdevstatus[i].expected_v4_state);
            g_assert_cmpint (v6_state, ==, test_netdevstatus[i].expected_v6_state);
        }
    }
}

/*****************************************************************************/

typedef struct {
    const gchar             *response;
    gboolean                 expect_error;
    MMQNetdevCtlConnectType  expected_connect_type;
    guint                    expected_cid;
    gboolean                 expected_connected;
} TestQNetdevCtlResponse;

static const TestQNetdevCtlResponse test_netdevctl[] = {
    { "+QNETDEVCTL: 0,0,0,0\r\n",         FALSE, MM_QNETDEVCTL_CONNECT_TYPE_DISCONNECTED, 0, FALSE },
    { "+QNETDEVCTL: 3,1,0,1\r\n",         FALSE, MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_AUTO, 1, TRUE },
    { "+QNETDEVSTATUS: adsfasdfasdf\r\n", TRUE,  MM_QNETDEVCTL_CONNECT_TYPE_DISCONNECTED, 0, FALSE },
    { "+QNETDEVSTATUS: 5,7,1,1\r\n",      TRUE,  MM_QNETDEVCTL_CONNECT_TYPE_DISCONNECTED, 0, FALSE },
    { "+QNETDEVSTATUS: 1,7,4,1\r\n",      TRUE,  MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_ONCE, 7, FALSE },
    { "+QNETDEVSTATUS: 1,7,0,4\r\n",      TRUE,  MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_ONCE, 7, FALSE },
    { "+QNETDEVSTATUS: 1,20,0,1\r\n",     TRUE,  MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_ONCE, 7, FALSE },
};

static void
test_mm_quectel_parse_qnetdevctl_response (void)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS (test_netdevctl); i++) {
        MMQNetdevCtlConnectType connect_type = MM_QNETDEVCTL_CONNECT_TYPE_DISCONNECTED;
        guint                   cid = 0;
        gboolean                connected = FALSE;
        g_autoptr(GError)       error = NULL;

        if (!mm_quectel_parse_qnetdevctl_response (test_netdevctl[i].response,
                                                   &connect_type,
                                                   &cid,
                                                   &connected,
                                                   &error)) {
            g_assert_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED);
        } else {
            g_assert_cmpuint (connect_type, ==, test_netdevctl[i].expected_connect_type);
            g_assert_cmpuint (cid, ==, test_netdevctl[i].expected_cid);
            g_assert_cmpuint (connected, ==, test_netdevctl[i].expected_connected);
        }
    }
}

/*****************************************************************************/

int main (int argc, char **argv)
{
    setlocale (LC_ALL, "");

    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/MM/quectel/ctzu", test_ctzu);
    g_test_add_func ("/MM/quectel/firmversion", test_firmversion);
    g_test_add_func ("/MM/quectel/parse_revision", test_parse_revision);
    g_test_add_func ("/MM/quectel/parse_qcfg", test_qcfg);
    g_test_add_func ("/MM/quectel/parse_qcfg_usbnet", test_qcfg_usbnet);
    g_test_add_func ("/MM/quectel/parse_qnetdevstatus", test_mm_quectel_parse_qnetdevstatus_response);
    g_test_add_func ("/MM/quectel/parse_qnetdevctl", test_mm_quectel_parse_qnetdevctl_response);

    return g_test_run ();
}
