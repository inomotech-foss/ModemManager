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

#include <ctype.h>
#include <glib.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

#include "mm-log.h"
#include "mm-modem-helpers.h"
#include "mm-modem-helpers-quectel.h"

gboolean
mm_quectel_parse_ctzu_test_response (const gchar  *response,
                                     gpointer      log_object,
                                     gboolean     *supports_disable,
                                     gboolean     *supports_enable,
                                     gboolean     *supports_enable_update_rtc,
                                     GError      **error)
{
    g_auto(GStrv)      split = NULL;
    g_autoptr(GArray)  modes = NULL;
    GError            *inner_error = NULL;
    guint              i;

    /*
     * Response may be:
     *   - +CTZU: (0,1)
     *   - +CTZU: (0,1,3)
     */

#define N_EXPECTED_GROUPS 1

    split = mm_split_string_groups (mm_strip_tag (response, "+CTZU:"));
    if (!split) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Couldn't split the +CTZU test response in groups");
        return FALSE;
    }

    if (g_strv_length (split) != N_EXPECTED_GROUPS) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Cannot parse +CTZU test response: invalid number of groups (%u != %u)",
                     g_strv_length (split), N_EXPECTED_GROUPS);
        return FALSE;
    }

    modes = mm_parse_uint_list (split[0], &inner_error);
    if (inner_error) {
        g_propagate_prefixed_error (error, inner_error, "Failed to parse integer list in +CTZU test response: ");
        return FALSE;
    }
    if (!modes) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Unexpected empty integer list in +CTZU test response: ");
        return FALSE;
    }

    *supports_disable = FALSE;
    *supports_enable = FALSE;
    *supports_enable_update_rtc = FALSE;

    for (i = 0; i < modes->len; i++) {
        guint mode;

        mode = g_array_index (modes, guint, i);
        switch (mode) {
            case 0:
                *supports_disable = TRUE;
                break;
            case 1:
                *supports_enable = TRUE;
                break;
            case 3:
                *supports_enable_update_rtc = TRUE;
                break;
            default:
                mm_obj_dbg (log_object, "unknown +CTZU mode: %u", mode);
                break;
        }
    }

    return TRUE;
}

/*****************************************************************************/
/* standard firmware info
 * Format of the string is:
 * "[main version]_[modem and app version]"
 * e.g. EM05GFAR07A07M1G_01.016.01.016
 */
#define QUECTEL_STD_FIRMWARE_VERSION_SEG  2

/* Format of the string is:
 * "modem_main.modem_minor.ap_main.ap_minor"
 * e.g. 01.016.01.016
 */
#define QUECTEL_STD_MODEM_AP_FIRMWARE_VER_SEG  4
#define QUECTEL_STD_MODEM_AP_FIRMWARE_VER_LEN  13

#define QUECTEL_MAIN_VERSION_INVALID_TAG   "00"
#define QUECTEL_MINOR_VERSION_INVALID_TAG  "000"

gboolean
mm_quectel_check_standard_firmware_version_valid (const gchar *std_str)
{
    gboolean      valid = TRUE;
    g_auto(GStrv) split_std_fw = NULL;
    g_auto(GStrv) split_modem_ap_fw = NULL;
    const gchar   *modem_ap_fw;

    if (std_str) {
        split_std_fw = g_strsplit (std_str, "_", QUECTEL_STD_FIRMWARE_VERSION_SEG);
        /* Quectel standard format of the [main version]_[modem and app version]
         * Sometimes we find that the [modem and app version] query is missing by [AT+QMGR]
         * for example: we expect EM05GFAR07A07M1G_01.016.01.016,but unexpected EM05GFAR07A07M1G_01.016.00.000 was returned
         * Quectel will check for this abnormal [modem and app version] and flag it
         */
        if (g_strv_length (split_std_fw) == QUECTEL_STD_FIRMWARE_VERSION_SEG) {
            modem_ap_fw = split_std_fw[1];
            if (strlen (modem_ap_fw) == QUECTEL_STD_MODEM_AP_FIRMWARE_VER_LEN) {
                split_modem_ap_fw = g_strsplit (modem_ap_fw, ".", QUECTEL_STD_MODEM_AP_FIRMWARE_VER_SEG);

                if (g_strv_length (split_modem_ap_fw) == QUECTEL_STD_MODEM_AP_FIRMWARE_VER_SEG &&
                    !g_strcmp0 (split_modem_ap_fw[2], QUECTEL_MAIN_VERSION_INVALID_TAG) &&
                    !g_strcmp0 (split_modem_ap_fw[3], QUECTEL_MINOR_VERSION_INVALID_TAG)){
                    valid = FALSE;
                }
            }
        }
    }
    return valid;
}

gboolean
mm_quectel_get_version_from_revision (const gchar  *revision,
                                      guint        *release,
                                      guint        *minor,
                                      GError      **error)
{
    g_autoptr(GRegex) version_regex = NULL;
    g_autoptr(GMatchInfo) match_info = NULL;

    version_regex = g_regex_new ("R(\\d+)A(\\d+)",
                                 G_REGEX_RAW | G_REGEX_OPTIMIZE,
                                 0,
                                 NULL);

    if (!g_regex_match (version_regex, revision, 0, &match_info)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Cannot parse revision version %s", revision);
        return FALSE;
    }
    if (!mm_get_uint_from_match_info (match_info, 1, release)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Couldn't get release version from revision %s", revision);
        return FALSE;
    }
    if (!mm_get_uint_from_match_info (match_info, 2, minor)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "Couldn't get minor version from revision %s", revision);
        return FALSE;
    }

    return TRUE;
}

gboolean
mm_quectel_is_profile_manager_supported (const gchar *revision,
                                         guint        release,
                                         guint        minor)
{
    guint i;
    static const struct {
        const gchar *revision_prefix;
        guint minimum_release;
        guint minimum_minor;
    } profile_support_map [] = {
        {"EC25", 6, 10},
    };

    for (i = 0; i < G_N_ELEMENTS (profile_support_map); ++i) {
        if (g_str_has_prefix (revision, profile_support_map[i].revision_prefix)) {
            guint minimum_release = profile_support_map[i].minimum_release;
            guint minimum_minor = profile_support_map[i].minimum_minor;

            return ((release > minimum_release) ||
                    (release == minimum_release && minor >= minimum_minor));
        }
    }

    return TRUE;
}

/*****************************************************************************/

/**
 * mm_quectel_parse_qcfg_test_response:
 * @response: An AT+QCFG=? response string.
 * @error: Return location for error or %NULL.
 *
 * Parses response to the AT+QCFG=? query.
 *
 * Returns: If the operation succeeded, a #GHashTable where each key is a
 * QCFG=? extended configuration setting name as a string, and each key is that
 * setting's values as a string. %NULL if the operation failed.
 */
GHashTable *
mm_quectel_parse_qcfg_test_response (const gchar  *response,
                                     GError      **error)
{
    g_autoptr(GRegex)      r = NULL;
    g_autoptr(GMatchInfo)  match_info = NULL;
    GHashTable            *hash;

    g_return_val_if_fail (response != NULL, NULL);

    /* Response contains multiple lines of name/values like:
     * +QCFG: "urc/ri/ring",("off","pulse"),(1-2000),(1-5)
     * +QCFG: "urc/ri/smsincoming",("off","pulse"),(1-2000),(1-5)
     * +QCFG: "urc/ri/other",("off","pulse"),(1-2000),(1-5)
     * +QCFG: "psm/urc",(0,1)
     * +QCFG: "cmux/urcport",(0-4)
     */

    r = g_regex_new ("\\+QCFG: \"([^\"]*)\",(.*)\\r\\n", G_REGEX_RAW, 0, NULL);
    g_assert (r);

    hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_free);

    if (g_regex_match (r, response, 0, &match_info)) {
        while (g_match_info_matches (match_info)) {
            gchar *name, *val;

            name = g_match_info_fetch (match_info, 1);
            val = g_match_info_fetch (match_info, 2);
            g_hash_table_insert (hash, name, g_strstrip (val));
            g_match_info_next (match_info, NULL);
        }
    }

    return hash;
}

/**
 * mm_quectel_parse_qcfg_usbnet_support:
 * @results: a #GHashTable returned by mm_quectel_parse_qcfg_test_response()
 * @error: Return location for error or %NULL
 *
 * Checks a parsed +QCFG response #GHashTable for "usbnet" support. Note that
 * this function may return %FALSE without setting @error if "usbnet" capability
 * is unsupported, but no parsing error occurred.
 *
 * Returns: %TRUE if "usbnet" capabilities are supported, %FALSE if "usbnet"
 * is unsupported or if an error occurred.
 */
gboolean
mm_quectel_parse_qcfg_usbnet_support (GHashTable *results,
                                      GError **error)
{
    g_autoptr(GRegex)      r = NULL;
    g_autoptr(GMatchInfo)  match_info = NULL;
    g_autofree gchar *     list = NULL;
    const gchar           *usbnet;
    g_autoptr(GArray)      vals = NULL;
    guint                  i;

    usbnet = g_hash_table_lookup (results, "usbnet");
    if (!usbnet) {
        /* No error; usbnet just unsupported */
        return FALSE;
    }

    r = g_regex_new ("\\((.*)\\)(?:\\r\\n)?", 0, 0, NULL);
    g_assert (r != NULL);

    if (!g_regex_match_full (r, usbnet, strlen (usbnet), 0, 0, &match_info, error))
        return FALSE;

    if (!g_match_info_matches (match_info)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Couldn't match usbnet response '%s'",
                     usbnet);
        return FALSE;
    }

    list = mm_get_string_unquoted_from_match_info (match_info, 1);
    if (!list) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_FAILED,
                     "Error parsing usbnet list string '%s'",
                     usbnet);
        return FALSE;
    }


    vals = mm_parse_uint_list (list, error);
    if (!vals)
        return FALSE;

    for (i = 0; i < vals->len; i++) {
        guint usbnet_val = g_array_index (vals, guint, i);

        /* Either ECM (1) or RNDIS (3) are acceptable */
        if (usbnet_val == 1 || usbnet_val == 3) {
            return TRUE;
        }
    }

    return FALSE;
}

/*****************************************************************************/

GRegex *
mm_quectel_new_qnetdevstatus_long_regex (void)
{
    GRegex *r;

    r = g_regex_new ("\\+QNETDEVSTATUS:\\s*(\\d+),(\\d+),(\\d+),(\\d+)\\r\\n", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    g_assert (r);
    return r;
}

static gboolean
get_uint_match (GMatchInfo   *match_info,
                guint         match_num,
                guint        *ret_val,
                guint         max_val,
                const gchar  *detail,
                GError      **error)
{
    guint num = 0;

    if (!mm_get_uint_from_match_info (match_info, match_num, &num)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "invalid %s value", detail);
        return FALSE;
    }
    if (num > max_val) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "unhandled %s value %d", detail, num);
        return FALSE;
    }
    *ret_val = num;
    return TRUE;
}

gboolean
mm_quectel_parse_one_qnetdevstatus (GMatchInfo               *match_info,
                                    MMQNetdevStatusCallState *call_state,
                                    gboolean                 *is_ipv4,
                                    GError                   **error)
{
    guint raw_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;
    guint raw_ip_type = 0;

    g_return_val_if_fail (match_info != NULL, FALSE);
    g_return_val_if_fail (call_state != NULL, FALSE);
    g_return_val_if_fail (is_ipv4 != NULL, FALSE);

    if (!get_uint_match (match_info,
                         2,
                         &raw_state,
                         MM_QNETDEVSTATUS_CALL_STATE_CONNECTED,
                         "+QNETDEVSTATUS state",
                         error))
        return FALSE;

    if (!get_uint_match (match_info,
                         3,
                         &raw_ip_type,
                         6,
                         "+QNETDEVSTATUS IP type",
                         error))
        return FALSE;

    if (raw_ip_type != 4 && raw_ip_type != 6) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "unhandled +QNETDEVSTATUS IP type value '%u'",
                     raw_ip_type);
        return FALSE;
    }

    if (call_state)
        *call_state = raw_state;
    if (is_ipv4)
        *is_ipv4 = (raw_ip_type == 4);
    return TRUE;
}

/**
 * mm_quectel_parse_qnetdevstatus_response:
 * @response: response to the AT+QNETDEVSTATUS? query
 * @v4_state: on return, IPv4 call state
 * @v6_state: on return, IPv6 call state
 * @error: Return location for error or %NULL
 *
 * Parses the output of the AT+QNETDEVSTATUS? query and returns IPv4 and/or IPv6
 * call state.
 *
 * Returns: %TRUE if parsing succeeded, %FALSE if an error occurred.
 */
gboolean
mm_quectel_parse_qnetdevstatus_response (const char                *response,
                                         MMQNetdevStatusCallState  *v4_state,
                                         MMQNetdevStatusCallState  *v6_state,
                                         GError                   **error)
{
    g_autoptr(GRegex)     r = NULL;
    g_autoptr(GMatchInfo) match_info = NULL;

    g_return_val_if_fail (response != NULL, FALSE);

    g_return_val_if_fail (v4_state != NULL, FALSE);
    *v4_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;

    g_return_val_if_fail (v6_state != NULL, FALSE);
    *v6_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;

    /* For many Quectel LTE modems (EG9x, EC2x, etc), response contains multiple
     * lines of name/values like:
     * +QNETDEVSTATUS: 1,2,4,1
     * +QNETDEVSTATUS: 1,2,6,1
     *
     * For others (EG060, RG200U) it returns IP addressing information, which
     * is not yet handled here.
     */

    r = mm_quectel_new_qnetdevstatus_long_regex ();

    if (!g_regex_match (r, response, 0, &match_info)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "failed to match +QNETDEVSTATUS response");
        return FALSE;
    }

    while (g_match_info_matches (match_info)) {
        MMQNetdevStatusCallState state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;
        gboolean                 is_ipv4 = FALSE;

        if (!mm_quectel_parse_one_qnetdevstatus (match_info, &state, &is_ipv4, error))
            return FALSE;

        if (is_ipv4)
            *v4_state = state;
        else
            *v6_state = state;

        g_match_info_next (match_info, NULL);
    }

    return TRUE;
}

/**
 * mm_quectel_parse_qnetdevctl_response:
 * @response: response to the AT+QNETDEVCTL? query
 * @connect_type: on return, the connect type
 * @cid: on return, the PDP context ID (if any) of the netdev connection
 * @connected: on return, whether or not the netdev is connected
 * @error: Return location for error or %NULL
 *
 * Parses the output of the AT+QNETDEVCTL? query and 
 *
 * Returns: %TRUE if parsing succeeded, %FALSE if an error occurred.
 */
gboolean
mm_quectel_parse_qnetdevctl_response (const char                *response,
                                      MMQNetdevCtlConnectType   *connect_type,
                                      guint                     *cid,
                                      gboolean                  *connected,
                                      GError                   **error)
{
    g_autoptr(GRegex)      r = NULL;
    g_autoptr(GMatchInfo)  match_info = NULL;
    guint                  raw_contype = 0;
    guint                  raw_connected = 0;
    guint                  raw_cid = 0;

    g_return_val_if_fail (response != NULL, FALSE);

    /* Possible responses include:
     * Disconnected:     +QNETDEVCTL: 0,0,0,0
     * PDP #1 connected: +QNETDEVCTL: 3,1,0,1
     */

    r = g_regex_new ("\\+QNETDEVCTL:\\s*(\\d+),(\\d+),(\\d+),(\\d+)", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    g_assert (r);

    if (!g_regex_match (r, response, 0, &match_info)) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                     "failed to match +QNETDEVCTL response");
        return FALSE;
    }

    if (!get_uint_match (match_info, 1, &raw_contype, MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_AUTO, "+QNETDEVCTL connect type", error))
        return FALSE;

    if (!get_uint_match (match_info, 2, &raw_cid, 15, "+QNETDEVCTL CID", error))
        return FALSE;

    if (!get_uint_match (match_info, 4, &raw_connected, 1, "+QNETDEVCTL connected", error))
        return FALSE;

    if (connect_type)
        *connect_type = raw_contype;
    if (cid)
        *cid = raw_cid;
    if (connected)
        *connected = raw_connected ? TRUE : FALSE;
    return TRUE;
}
