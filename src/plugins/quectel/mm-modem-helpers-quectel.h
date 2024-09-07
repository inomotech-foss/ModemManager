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
 */

#ifndef MM_MODEM_HELPERS_QUECTEL_H
#define MM_MODEM_HELPERS_QUECTEL_H

#include <glib.h>

#include <ModemManager.h>
#define _LIBMM_INSIDE_MM
#include <libmm-glib.h>

gboolean mm_quectel_parse_ctzu_test_response (const gchar  *response,
                                              gpointer      log_object,
                                              gboolean     *supports_disable,
                                              gboolean     *supports_enable,
                                              gboolean     *supports_enable_update_rtc,
                                              GError      **error);

gboolean mm_quectel_check_standard_firmware_version_valid (const gchar *std_str);

gboolean mm_quectel_get_version_from_revision (const gchar  *revision,
                                               guint        *release,
                                               guint        *minor,
                                               GError      **error);

gboolean mm_quectel_is_profile_manager_supported (const gchar *revision,
                                                  guint        release,
                                                  guint        minor);

GHashTable *mm_quectel_parse_qcfg_test_response (const gchar  *response,
                                                 GError      **error);

gboolean mm_quectel_parse_qcfg_usbnet_support (GHashTable *results,
                                               GError **error);

GRegex *mm_quectel_new_qnetdevstatus_long_regex (void);

/**
 * MMQNetdevStatusCallState:
 * @MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED: call is not active.
 * @MM_QNETDEVSTATUS_CALL_STATE_READY: call is active and modem is waiting for host to perform DHCP/RA.
 * @MM_QNETDEVSTATUS_CALL_STATE_CONNECTED: call is active and host has performed IP addressing.
 *
 * Values describing the usbnet call state.
 */
typedef enum {
    MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED = 0,
    MM_QNETDEVSTATUS_CALL_STATE_READY        = 1,
    MM_QNETDEVSTATUS_CALL_STATE_CONNECTED    = 2,
} MMQNetdevStatusCallState;

gboolean mm_quectel_parse_one_qnetdevstatus (GMatchInfo               *match_info,
                                             MMQNetdevStatusCallState *call_state,
                                             gboolean                 *is_ipv4,
                                             GError                   **error);

gboolean mm_quectel_parse_qnetdevstatus_response (const char                *response,
                                                  MMQNetdevStatusCallState  *v4_state,
                                                  MMQNetdevStatusCallState  *v6_state,
                                                  GError                   **error);

/**
 * MMQNetdevCtlConnectType:
 * @MM_QNETDEVCTL_CONNECT_TYPE_DISCONNECTED: not connected/do not connect.
 * @MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_ONCE: connect to the network once.
 * @MM_QNETDEVCTL_CONNECT_TYPE_UNUSED: placeholder for invalid/unused value.
 * @MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_AUTO: connect to the network automatically.
 *
 * Values describing the usbnet connect type.
 */
typedef enum {
    MM_QNETDEVCTL_CONNECT_TYPE_DISCONNECTED = 0,
    MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_ONCE = 1,
    MM_QNETDEVCTL_CONNECT_TYPE_UNUSED       = 2,
    MM_QNETDEVCTL_CONNECT_TYPE_CONNECT_AUTO = 3,
} MMQNetdevCtlConnectType;

gboolean mm_quectel_parse_qnetdevctl_response (const char               *response,
                                               MMQNetdevCtlConnectType  *connect_type,
                                               guint                    *cid,
                                               gboolean                 *connected,
                                               GError                  **error);

#endif  /* MM_MODEM_HELPERS_QUECTEL_H */
