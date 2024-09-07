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
 * Copyright (C) 2022 Disruptive Technologies Research AS
 * Copyright (C) 2024 JUCR GmbH
 */

#include <config.h>

#include "mm-broadband-bearer-quectel-ecm.h"
#include "mm-broadband-modem-quectel.h"
#include "mm-modem-helpers-quectel.h"
#include "mm-daemon-enums-types.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-log.h"

G_DEFINE_TYPE (MMBroadbandBearerQuectelEcm, mm_broadband_bearer_quectel_ecm, MM_TYPE_BROADBAND_BEARER)

struct _MMBroadbandBearerQuectelEcmPrivate {
    /* If %TRUE use $QCRMCALL for call start/stop and +QNETDEVSTATUS for status.
     * If %FALSE use +QNETDEVCTL for call start/stop/status.
     */
    gboolean  use_qcrmcall;
    GTask    *connect_pending;
    guint     connect_timeout_id;
    gint      profile_id;
};

enum {
    PROP_0,
    PROP_USE_QCRMCALL,
    PROP_LAST
};

static void process_pending_connect_attempt (MMBroadbandBearerQuectelEcm   *self,
                                             MMBearerConnectionStatus       status);

/*****************************************************************************/
/* Connection status monitoring                                              */

static MMBearerConnectionStatus
load_connection_status_finish (MMBaseBearer  *bearer,
                               GAsyncResult  *res,
                               GError       **error)
{
    GError *inner_error = NULL;
    gssize value;

    value = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return MM_BEARER_CONNECTION_STATUS_UNKNOWN;
    }
    return (MMBearerConnectionStatus) value;
}

static void
load_qnetdevstatus_ready (MMBaseModem  *modem,
                          GAsyncResult *res,
                          GTask        *task)
{
    MMBroadbandBearer        *self;
    GError                   *error = NULL;
    const gchar              *response;
    MMBearerIpFamily          bearer_ipf;

    self = g_task_get_source_object (task);
    bearer_ipf = mm_bearer_properties_get_ip_type (mm_base_bearer_peek_config (MM_BASE_BEARER (self)));

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (response) {
        MMQNetdevStatusCallState  v4_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;
        MMQNetdevStatusCallState  v6_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;
        gboolean                  connected = FALSE;

        if (mm_quectel_parse_qnetdevstatus_response (response, &v4_state, &v6_state, &error)) {
            switch (bearer_ipf) {
            case MM_BEARER_IP_FAMILY_IPV4:
                connected = (v4_state >= MM_QNETDEVSTATUS_CALL_STATE_READY);
                break;
            case MM_BEARER_IP_FAMILY_IPV6:
                connected = (v6_state >= MM_QNETDEVSTATUS_CALL_STATE_READY);
                break;
            case MM_BEARER_IP_FAMILY_IPV4V6:
                connected = (v4_state >= MM_QNETDEVSTATUS_CALL_STATE_READY
                             && v6_state >= MM_QNETDEVSTATUS_CALL_STATE_READY);
                break;
            case MM_BEARER_IP_FAMILY_NONE:
            case MM_BEARER_IP_FAMILY_NON_IP:
            case MM_BEARER_IP_FAMILY_ANY:
            default:
                break;
            }

            g_task_return_int (task,
                               connected ? MM_BEARER_CONNECTION_STATUS_CONNECTED :
                                   MM_BEARER_CONNECTION_STATUS_DISCONNECTED);
            g_object_unref (task);
            return;
        }
    }

    g_task_return_error (task, error);
    g_object_unref (task);
}


static void
load_qnetdevctl_ready (MMBaseModem  *modem,
                       GAsyncResult *res,
                       GTask        *task)
{
    MMBroadbandBearerQuectelEcm *self;
    GError                      *error = NULL;
    const gchar                 *response;

    self = g_task_get_source_object (task);

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (response) {
        guint    cid = 0;
        gboolean connected = FALSE;

        if (mm_quectel_parse_qnetdevctl_response (response, NULL, &cid, &connected, &error)) {
            if (cid == 0 || cid != (guint) self->priv->profile_id)
                connected = FALSE;

            g_task_return_int (task,
                               connected ? MM_BEARER_CONNECTION_STATUS_CONNECTED :
                                   MM_BEARER_CONNECTION_STATUS_DISCONNECTED);
            g_object_unref (task);
            return;
        }
    }

    g_task_return_error (task, error);
    g_object_unref (task);
}

static void
load_connection_status (MMBaseBearer        *bearer,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
    MMBroadbandBearerQuectelEcm *self = MM_BROADBAND_BEARER_QUECTEL_ECM (bearer);
    GTask       *task;
    MMBaseModem *modem = NULL;

    task = g_task_new (bearer, NULL, callback, user_data);

    g_object_get (MM_BASE_BEARER (bearer),
                  MM_BASE_BEARER_MODEM, &modem,
                  NULL);

    if (self->priv->use_qcrmcall) {
        mm_base_modem_at_command (modem,
                                  "+QNETDEVSTATUS?",
                                  3,
                                  FALSE, /* allow_cached */
                                  (GAsyncReadyCallback)load_qnetdevstatus_ready,
                                  task);
    } else {
        mm_base_modem_at_command (modem,
                                  "+QNETDEVCTL?",
                                  3,
                                  FALSE, /* allow_cached */
                                  (GAsyncReadyCallback)load_qnetdevctl_ready,
                                  task);
    }

    g_object_unref (modem);
}

/*****************************************************************************/
/* 3GPP Connect                                                              */

typedef struct {
    MMBroadbandModem *modem;
    MMPortSerialAt   *primary;
    MMPortSerialAt   *secondary;
} ConnectContext;

static void
connect_context_free (ConnectContext *ctx)
{
    g_clear_object (&ctx->modem);
    g_clear_object (&ctx->primary);
    g_clear_object (&ctx->secondary);
    g_slice_free (ConnectContext, ctx);
}

static MMBearerConnectResult *
connect_3gpp_finish (MMBroadbandBearer  *self,
                     GAsyncResult       *res,
                     GError            **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
parent_connect_3gpp_ready (MMBroadbandBearer *self,
                           GAsyncResult      *res,
                           GTask             *task)
{
    GError                *error = NULL;
    MMBearerConnectResult *result;

    result = MM_BROADBAND_BEARER_CLASS (mm_broadband_bearer_quectel_ecm_parent_class)->connect_3gpp_finish (self, res, &error);
    if (result)
        g_task_return_pointer (task, result, (GDestroyNotify) mm_bearer_connect_result_unref);
    else
        g_task_return_error (task, error);
    g_object_unref (task);
}

static void
disconnect_3gpp_ready (MMBroadbandBearer *self,
                       GAsyncResult      *res,
                       GTask             *task)
{
    GError         *error = NULL;
    gboolean        result;
    ConnectContext *ctx;

    result = MM_BROADBAND_BEARER_GET_CLASS (self)->disconnect_3gpp_finish (self, res, &error);
    if (!result) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx = g_task_get_task_data (task);
    MM_BROADBAND_BEARER_CLASS (mm_broadband_bearer_quectel_ecm_parent_class)->connect_3gpp (
        self,
        ctx->modem,
        ctx->primary,
        ctx->secondary,
        g_task_get_cancellable (task),
        (GAsyncReadyCallback) parent_connect_3gpp_ready,
        task);
}

static void
common_connect_check (GTask *task, gint profile_id, gboolean connected)
{
    MMBroadbandBearer        *self;
    ConnectContext           *ctx;

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    if (connected) {
        /* usbnet is already active, disconnect first. */
        mm_obj_dbg (self, "usbnet active, tearing down existing connection...");
        MM_BROADBAND_BEARER_GET_CLASS (self)->disconnect_3gpp (
            MM_BROADBAND_BEARER (self),
            ctx->modem,
            ctx->primary,
            ctx->secondary,
            NULL, /* data port */
            profile_id,
            (GAsyncReadyCallback) disconnect_3gpp_ready,
            task);
        return;
    }

    /* Execute the regular connection flow if usbnet is inactive. */
    mm_obj_dbg (self, "usbnet inactive; connecting...");
    MM_BROADBAND_BEARER_CLASS (mm_broadband_bearer_quectel_ecm_parent_class)->connect_3gpp (
        MM_BROADBAND_BEARER (self),
        ctx->modem,
        ctx->primary,
        ctx->secondary,
        g_task_get_cancellable (task),
        (GAsyncReadyCallback) parent_connect_3gpp_ready,
        task);
}

static void
qnetdevstatus_check_ready (MMBaseModem  *modem,
                           GAsyncResult *res,
                           GTask        *task)
{
    GError                   *error = NULL;
    const gchar              *response;
    MMQNetdevStatusCallState  v4_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;
    MMQNetdevStatusCallState  v6_state = MM_QNETDEVSTATUS_CALL_STATE_DISCONNECTED;

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (!response) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!mm_quectel_parse_qnetdevstatus_response (response, &v4_state, &v6_state, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    common_connect_check (task,
                          0,
                          (v4_state >= MM_QNETDEVSTATUS_CALL_STATE_READY
                           || v6_state >= MM_QNETDEVSTATUS_CALL_STATE_READY));
}


static void
qnetdevctl_check_ready (MMBaseModem  *modem,
                        GAsyncResult *res,
                        GTask        *task)
{
    GError            *error = NULL;
    const gchar       *response;
    guint              cid = 0;
    gboolean           connected = FALSE;

    response = mm_base_modem_at_command_finish (modem, res, &error);
    if (response) {
        if (mm_quectel_parse_qnetdevctl_response (response, NULL, &cid, &connected, &error)) {
            /* If the CID didn't match or was 0, assume disconnected */
            common_connect_check (task, cid, connected);
            return;
        }
    }

    g_task_return_error (task, error);
    g_object_unref (task);
}

static void
connect_3gpp (MMBroadbandBearer   *_self,
              MMBroadbandModem    *modem,
              MMPortSerialAt      *primary,
              MMPortSerialAt      *secondary,
              GCancellable        *cancellable,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
    MMBroadbandBearerQuectelEcm *self = MM_BROADBAND_BEARER_QUECTEL_ECM (_self);
    ConnectContext              *ctx;
    GTask                       *task;

    ctx            = g_slice_new0 (ConnectContext);
    ctx->modem     = g_object_ref (modem);
    ctx->primary   = g_object_ref (primary);
    ctx->secondary = secondary ? g_object_ref (secondary) : NULL;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) connect_context_free);

    /* First, we must check whether usbnet is already active */
    if (self->priv->use_qcrmcall) {
        mm_base_modem_at_command (MM_BASE_MODEM (modem),
                                  "+QNETDEVSTATUS?",
                                  3,
                                  FALSE, /* allow_cached */
                                  (GAsyncReadyCallback)qnetdevstatus_check_ready,
                                  task);
    } else {
        mm_base_modem_at_command (MM_BASE_MODEM (modem),
                                  "+QNETDEVCTL?",
                                  3,
                                  FALSE, /* allow_cached */
                                  (GAsyncReadyCallback)qnetdevctl_check_ready,
                                  task);
    }
}

/*****************************************************************************/
/* 3GPP IP config retrieval (sub-step of the 3GPP Connection sequence) */

typedef struct {
    MMPort           *data;
    MMBearerIpFamily  ip_family;
} GetIpConfig3gppContext;

static void
get_ip_config_context_free (GetIpConfig3gppContext *ctx)
{
    g_object_unref (ctx->data);
    g_free (ctx);
}

static gboolean
get_ip_config_3gpp_finish (MMBroadbandBearer *self,
                           GAsyncResult *res,
                           MMBearerIpConfig **ipv4_config,
                           MMBearerIpConfig **ipv6_config,
                           GError **error)
{
    MMBearerConnectResult *configs;
    MMBearerIpConfig *ipv4, *ipv6;

    configs = g_task_propagate_pointer (G_TASK (res), error);
    if (!configs)
        return FALSE;

    ipv4 = mm_bearer_connect_result_peek_ipv4_config (configs);
    ipv6 = mm_bearer_connect_result_peek_ipv6_config (configs);
    g_assert (ipv4 || ipv6);
    if (ipv4_config && ipv4)
        *ipv4_config = g_object_ref (ipv4);
    if (ipv6_config && ipv6)
        *ipv6_config = g_object_ref (ipv6);

    mm_bearer_connect_result_unref (configs);
    return TRUE;
}

static void
get_hwaddress_ready (MMPortNet *port,
                     GAsyncResult *res,
                     GTask *task)
{
    GetIpConfig3gppContext *ctx;
    GByteArray             *hwaddr;
    MMBearerIpConfig       *ipv4_config = NULL;
    MMBearerIpConfig       *ipv6_config = NULL;
    GError                 *error = NULL;
    MMBearerConnectResult  *connect_result;

    ctx = g_task_get_task_data (task);

    hwaddr = mm_port_net_get_hwaddress_finish (port, res, &error);
    if (!hwaddr) {
        g_task_return_error (task, error);
        goto out;
    }

    if (ctx->ip_family & MM_BEARER_IP_FAMILY_IPV4 ||
            ctx->ip_family & MM_BEARER_IP_FAMILY_IPV4V6) {
        ipv4_config = mm_bearer_ip_config_new ();
        mm_bearer_ip_config_set_method (ipv4_config, MM_BEARER_IP_METHOD_DHCP);
    }

    if (ctx->ip_family & MM_BEARER_IP_FAMILY_IPV6 ||
            ctx->ip_family & MM_BEARER_IP_FAMILY_IPV4V6) {
        g_autofree gchar *lladdr;

        ipv6_config = mm_bearer_ip_config_new ();
        mm_bearer_ip_config_set_method (ipv6_config, MM_BEARER_IP_METHOD_DHCP);

        lladdr = g_strdup_printf ("fe80::%02x%02x:%02xff:fe%02x:%02x%02x",
                                  hwaddr->data[0] ^ 2, hwaddr->data[1],
                                  hwaddr->data[2], hwaddr->data[3],
                                  hwaddr->data[4], hwaddr->data[5]);

        mm_bearer_ip_config_set_address (ipv6_config, lladdr);
        mm_bearer_ip_config_set_prefix (ipv6_config, 64);
    }

    if (!ipv4_config && !ipv6_config) {
        error = g_error_new_literal (MM_CORE_ERROR,
                                     MM_CORE_ERROR_FAILED,
                                     "Couldn't generate IP config: invalid IP family");
        g_task_return_error (task, error);
        goto out;
    }

    connect_result = mm_bearer_connect_result_new (MM_PORT (ctx->data),
                                                   ipv4_config,
                                                   ipv6_config);
    g_task_return_pointer (task,
                           connect_result,
                           (GDestroyNotify)mm_bearer_connect_result_unref);

out:
    g_object_unref (task);
    g_clear_object (&ipv4_config);
    g_clear_object (&ipv6_config);
}


static void
get_ip_config_3gpp (MMBroadbandBearer *self,
                    MMBroadbandModem *modem,
                    MMPortSerialAt *primary,
                    MMPortSerialAt *secondary,
                    MMPort *data,
                    guint cid,
                    MMBearerIpFamily ip_family,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GetIpConfig3gppContext *ctx;
    GTask                  *task;

    ctx = g_new0 (GetIpConfig3gppContext, 1);
    ctx->data = g_object_ref (data);
    ctx->ip_family = ip_family;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)get_ip_config_context_free);

    mm_port_net_get_hwaddress (MM_PORT_NET (ctx->data),
                               NULL,
                               (GAsyncReadyCallback) get_hwaddress_ready,
                               task);
}

/*****************************************************************************/
/* Dial context and task                                                     */

typedef struct {
    MMBroadbandModem *modem;
    guint             cid;
    MMPort           *data;
} DialContext;

static void
dial_task_free (DialContext *ctx)
{
    if (ctx->data)
        g_object_unref (ctx->data);
    g_slice_free (DialContext, ctx);
}

static GTask *
dial_task_new (MMBroadbandBearerQuectelEcm *self,
               MMBroadbandModem         *modem,
               MMPortSerialAt           *primary,
               guint                     cid,
               GCancellable             *cancellable,
               GAsyncReadyCallback       callback,
               gpointer                  user_data)
{
    DialContext *ctx;
    GTask       *task;

    ctx          = g_slice_new0 (DialContext);
    ctx->cid     = cid;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) dial_task_free);

    ctx->data = mm_base_modem_get_best_data_port (MM_BASE_MODEM (modem), MM_PORT_TYPE_NET);
    if (!ctx->data) {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_NOT_FOUND,
                                 "No valid data port found to launch connection");
        g_object_unref (task);
        return NULL;
    }

    return task;
}

/*****************************************************************************/

static void
report_connection_status (MMBaseBearer             *_self,
                          MMBearerConnectionStatus  status,
                          const GError             *connection_error)
{
    MMBroadbandBearerQuectelEcm *self = MM_BROADBAND_BEARER_QUECTEL_ECM (_self);

    g_assert (status == MM_BEARER_CONNECTION_STATUS_CONNECTED ||
              status == MM_BEARER_CONNECTION_STATUS_CONNECTION_FAILED ||
              status == MM_BEARER_CONNECTION_STATUS_DISCONNECTED);

    /* Process pending connection attempt */
    if (self->priv->connect_pending) {
        process_pending_connect_attempt (self, status);
        return;
    }

    mm_obj_dbg (self, "received spontaneous usbnet status (%s)", mm_bearer_connection_status_get_string (status));

    /* Received a random 'DISCONNECTED'...*/
    if (status == MM_BEARER_CONNECTION_STATUS_DISCONNECTED ||
        status == MM_BEARER_CONNECTION_STATUS_CONNECTION_FAILED) {
        /* If no connection/disconnection attempt on-going, make sure we mark ourselves as
         * disconnected. Make sure we only pass 'DISCONNECTED' to the parent */
        MM_BASE_BEARER_CLASS (mm_broadband_bearer_quectel_ecm_parent_class)->report_connection_status (
            _self,
            MM_BEARER_CONNECTION_STATUS_DISCONNECTED,
            connection_error);
    }
}

/*****************************************************************************/
/* 3GPP Dialing (sub-step of the 3GPP Connection sequence)                   */

static MMPort *
dial_3gpp_finish (MMBroadbandBearer *self,
                  GAsyncResult *res,
                  GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
process_pending_connect_attempt (MMBroadbandBearerQuectelEcm *self,
                                 MMBearerConnectionStatus     status)
{
    GTask       *task;
    DialContext *ctx;

    /* Recover task and remove both cancellation and timeout (if any)*/
    g_assert (self->priv->connect_pending);
    task = g_steal_pointer (&self->priv->connect_pending);
    ctx = g_task_get_task_data (task);

    if (self->priv->connect_timeout_id) {
        g_source_remove (self->priv->connect_timeout_id);
        self->priv->connect_timeout_id = 0;
    }

    /* If we wanted to get cancelled before and now we couldn't connect,
     * use the cancelled error and return */
    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    /* Received connect notification during a connection attempt? */
    if (status == MM_BEARER_CONNECTION_STATUS_CONNECTED) {
        /* Cache the CID for later */
        self->priv->profile_id = ctx->cid;
        g_task_return_pointer (task, g_object_ref (ctx->data), g_object_unref);
        g_object_unref (task);
        return;
    }

    /* Otherwise, received disconnect during a connection attempt? */
    g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Call setup failed");
    g_object_unref (task);
}

static gboolean
connect_timed_out_cb (MMBroadbandBearerQuectelEcm *self)
{
    GTask       *task;

    /* Cleanup timeout ID */
    self->priv->connect_timeout_id = 0;

    /* Recover task and own it */
    task = g_steal_pointer (&self->priv->connect_pending);
    g_assert (task);

    /* When reset is requested, it was either cancelled or an error was stored */
    if (!g_task_return_error_if_cancelled (task)) {
        g_task_return_new_error (task,
                                 MM_MOBILE_EQUIPMENT_ERROR,
                                 MM_MOBILE_EQUIPMENT_ERROR_NETWORK_TIMEOUT,
                                 "Connection attempt timed out");
    }

    g_object_unref (task);

    return G_SOURCE_REMOVE;
}

static void
usbnet_activate_ready (MMBaseModem                 *modem,
                       GAsyncResult                *res,
                       MMBroadbandBearerQuectelEcm *self)
{
    GTask       *task;
    GError      *error = NULL;

    task = g_steal_pointer (&self->priv->connect_pending);

    /* Try to recover the connection context. If none found, it means the
     * context was already completed and we have nothing else to do. */
    if (!task) {
        mm_obj_dbg (self, "connection context was finished already by an unsolicited message");
        /* Run _finish() to finalize the async call, even if we don't care
         * the result */
        mm_base_modem_at_command_finish (modem, res, NULL);
        goto out;
    }

    /* Errors on the dial command are fatal */
    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        goto out;
    }

    /* Track again */
    self->priv->connect_pending = task;

    /* We will now setup a timeout and keep the context in the bearer's private.
     * Reports of modem being connected will arrive via unsolicited messages.
     * This timeout should be long enough. Actually... ideally should never get
     * reached. */
    self->priv->connect_timeout_id = g_timeout_add_seconds (MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                                                            (GSourceFunc)connect_timed_out_cb,
                                                            self);

 out:
    /* Balance refcount with the extra ref we passed to mm_base_modem_at_command() */
    g_object_unref (self);
}

static guint
qcrmcall_ip_type_from_bearer (MMBaseBearer *self, GError **error)
{
    MMBearerProperties *config;
    MMBearerIpFamily    ip_family;

    config = mm_base_bearer_peek_config (MM_BASE_BEARER (self));
    ip_family = mm_bearer_properties_get_ip_type (config);

    if (ip_family == MM_BEARER_IP_FAMILY_IPV4)
        return 1;
    else if (ip_family == MM_BEARER_IP_FAMILY_IPV6)
        return 2;
    else if (ip_family == MM_BEARER_IP_FAMILY_IPV4V6)
        return 3;

    g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                 "invalid bearer IP family %d", ip_family);
    return 0;
}

static void
dial_3gpp (MMBroadbandBearer  *_self,
           MMBaseModem        *modem,
           MMPortSerialAt     *primary,
           guint               cid,
           GCancellable       *cancellable,
           GAsyncReadyCallback callback,
           gpointer            user_data)
{
    MMBroadbandBearerQuectelEcm *self = MM_BROADBAND_BEARER_QUECTEL_ECM (_self);
    GTask                       *task;
    g_autofree gchar            *cmd = NULL;

    task = dial_task_new (MM_BROADBAND_BEARER_QUECTEL_ECM (self),
                          MM_BROADBAND_MODEM (modem),
                          primary,
                          cid,
                          cancellable,
                          callback,
                          user_data);
    if (!task)
        return;

    if (self->priv->use_qcrmcall) {
        guint   ip_type;
        GError *error = NULL;

        ip_type = qcrmcall_ip_type_from_bearer (MM_BASE_BEARER (self), &error);
        if (!ip_type) {
            g_task_return_error (task, error);
            g_object_unref (task);
            return;
        }

        /* AT$QCRMCALL=<start/stop>,1[,<IP_type>[,<tech_pref>[,<profile_num>]]
         * <IP_type>: 1 = V4, 2 = V6, 3 = V4V6
         * <tech_pref>: 1 = 3GPP2, 2 = 3GPP
         */
        cmd = g_strdup_printf ("$QCRMCALL=1,1,%u,2,%u", ip_type, cid);
    } else {
        /* AT+QNETDEVCTL=<start/stop>,<cid>,<urc enable> */
        cmd = g_strdup_printf ("+QNETDEVCTL=1,%u,1", cid);
    }

    /* The unsolicited response to the connect request may come before the OK
     * does. We keep the connection task in the bearer private data so
     * that it is accessible from the unsolicited message handler. Note
     * also that we do NOT pass the ctx to the GAsyncReadyCallback, as it
     * may not be valid any more when the callback is called (it may be
     * already completed in the unsolicited handling) */
    g_assert (self->priv->connect_pending == NULL);
    self->priv->connect_pending = task;

    mm_base_modem_at_command (modem,
                              cmd,
                              MM_BASE_BEARER_DEFAULT_CONNECTION_TIMEOUT,
                              FALSE, /* allow_cached */
                              (GAsyncReadyCallback) usbnet_activate_ready,
                              g_object_ref (self)); /* we pass the bearer object! */
}

/*****************************************************************************/
/* 3GPP Disconnect sequence                                                  */

static gboolean
disconnect_3gpp_finish (MMBroadbandBearer *self,
                        GAsyncResult *res,
                        GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
disconnect_cgact_ready (MMBaseModem *modem,
                        GAsyncResult *res,
                        GTask *task)
{
    GError *error = NULL;

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
    } else {
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
usbnet_deactivate_ready (MMBaseModem *modem,
                         GAsyncResult *res,
                         GTask *task)
{
    guint              cid;
    g_autofree gchar  *cmd = NULL;
    GError            *error = NULL;

    cid = GPOINTER_TO_UINT (g_task_get_task_data (task));

    if (!mm_base_modem_at_command_finish (modem, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    cmd = g_strdup_printf ("+CGACT=0,%d", cid);
    mm_base_modem_at_command (MM_BASE_MODEM (modem),
                              cmd,
                              MM_BASE_BEARER_DEFAULT_DISCONNECTION_TIMEOUT,
                              FALSE, /* allow_cached */
                              (GAsyncReadyCallback) disconnect_cgact_ready,
                              task);
}

static void
disconnect_3gpp (MMBroadbandBearer *_self,
                 MMBroadbandModem *modem,
                 MMPortSerialAt *primary,
                 MMPortSerialAt *secondary,
                 MMPort *data,
                 guint cid,
                 GAsyncReadyCallback callback,
                 gpointer user_data)
{
    MMBroadbandBearerQuectelEcm *self = MM_BROADBAND_BEARER_QUECTEL_ECM (_self);
    GTask                       *task;
    g_autofree gchar            *cmd = NULL;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, GUINT_TO_POINTER (cid), NULL);

    if (self->priv->use_qcrmcall) {
        guint   ip_type;
        GError *error = NULL;

        ip_type = qcrmcall_ip_type_from_bearer (MM_BASE_BEARER (self), &error);
        if (!ip_type) {
            g_task_return_error (task, error);
            g_object_unref (task);
            return;
        }

        /* AT$QCRMCALL=<start/stop>,1[,<IP_type>[,<tech_pref>[,<profile_num>]]
         * <IP_type>: 1 = V4, 2 = V6, 3 = V4V6
         * <tech_pref>: 1 = 3GPP2, 2 = 3GPP
         */
        if (cid > 0)
            cmd = g_strdup_printf ("$QCRMCALL=0,1,%u,2,%u", ip_type, cid);
        else
            cmd = g_strdup_printf ("$QCRMCALL=0,1");
    } else {
        /* We should always have the CID when using +QNETDEVCTL */
        g_assert (cid > 0);
        cmd = g_strdup_printf ("+QNETDEVCTL=0,%u,1", cid);
    }

    mm_base_modem_at_command (MM_BASE_MODEM (modem),
                              cmd,
                              MM_BASE_BEARER_DEFAULT_DISCONNECTION_TIMEOUT,
                              FALSE, /* allow_cached */
                              (GAsyncReadyCallback) usbnet_deactivate_ready,
                              task);
}

/*****************************************************************************/

#define MM_BROADBAND_BEARER_QUECTEL_ECM_USE_QCRMCALL "use-qcrmcall"

MMBaseBearer *
mm_broadband_bearer_quectel_ecm_new_finish (GAsyncResult *res,
                                            GError **error)
{
    GObject *bearer;
    GObject *source;

    source = g_async_result_get_source_object (res);
    bearer = g_async_initable_new_finish (G_ASYNC_INITABLE (source), res, error);
    g_object_unref (source);

    if (!bearer)
        return NULL;

    /* Only export valid bearers */
    mm_base_bearer_export (MM_BASE_BEARER (bearer));

    return MM_BASE_BEARER (bearer);
}

void
mm_broadband_bearer_quectel_ecm_new (MMBroadbandModemQuectel *modem,
                                     MMBearerProperties *config,
                                     gboolean use_qcrmcall,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
    g_async_initable_new_async (
        MM_TYPE_BROADBAND_BEARER_QUECTEL_ECM,
        G_PRIORITY_DEFAULT,
        cancellable,
        callback,
        user_data,
        MM_BASE_BEARER_MODEM, modem,
        MM_BASE_BEARER_CONFIG, config,
        MM_BROADBAND_BEARER_QUECTEL_ECM_USE_QCRMCALL, use_qcrmcall,
        NULL);
}

static void
set_property (GObject *object,
              guint prop_id,
              const GValue *value,
              GParamSpec *pspec)
{
    MMBroadbandBearerQuectelEcm *self = MM_BROADBAND_BEARER_QUECTEL_ECM (object);

    switch (prop_id) {
    case PROP_USE_QCRMCALL:
        self->priv->use_qcrmcall = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
get_property (GObject *object,
              guint prop_id,
              GValue *value,
              GParamSpec *pspec)
{
    MMBroadbandBearerQuectelEcm *self = MM_BROADBAND_BEARER_QUECTEL_ECM (object);

    switch (prop_id) {
    case PROP_USE_QCRMCALL:
        g_value_set_boolean (value, self->priv->use_qcrmcall);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
mm_broadband_bearer_quectel_ecm_init (MMBroadbandBearerQuectelEcm *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self),
                                              MM_TYPE_BROADBAND_BEARER_QUECTEL_ECM,
                                              MMBroadbandBearerQuectelEcmPrivate);
}

static void
mm_broadband_bearer_quectel_ecm_class_init (MMBroadbandBearerQuectelEcmClass *klass)
{
    GObjectClass           *object_class           = G_OBJECT_CLASS (klass);
    MMBaseBearerClass *base_bearer_class           = MM_BASE_BEARER_CLASS (klass);
    MMBroadbandBearerClass *broadband_bearer_class = MM_BROADBAND_BEARER_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandBearerQuectelEcmPrivate));

    object_class->set_property = set_property;
    object_class->get_property = get_property;

    base_bearer_class->load_connection_status = load_connection_status;
    base_bearer_class->load_connection_status_finish = load_connection_status_finish;
    base_bearer_class->report_connection_status = report_connection_status;

    broadband_bearer_class->connect_3gpp = connect_3gpp;
    broadband_bearer_class->connect_3gpp_finish = connect_3gpp_finish;
    broadband_bearer_class->dial_3gpp = dial_3gpp;
    broadband_bearer_class->dial_3gpp_finish = dial_3gpp_finish;
    broadband_bearer_class->get_ip_config_3gpp = get_ip_config_3gpp;
    broadband_bearer_class->get_ip_config_3gpp_finish = get_ip_config_3gpp_finish;
    broadband_bearer_class->disconnect_3gpp = disconnect_3gpp;
    broadband_bearer_class->disconnect_3gpp_finish = disconnect_3gpp_finish;

    g_object_class_install_property (object_class, PROP_USE_QCRMCALL,
        g_param_spec_boolean (MM_BROADBAND_BEARER_QUECTEL_ECM_USE_QCRMCALL,
                              "UseQcrmcall",
                              "Whether the modem uses $QCRMCALL to start a data call.",
                              FALSE,
                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}
