/*
 * Copyright (c) 2017, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib.h>
#include <inttypes.h>
#include <poll.h>
#include <string.h>

#include <sapi/tpm20.h>

#include "tabrmd.h"
#include "tcti-tabrmd.h"
#include "tcti-tabrmd-priv.h"
#include "tpm2-header.h"
#include "util.h"
#include "gtlsconsoleinteraction.h"

static TSS2_RC
tss2_tcti_tabrmd_transmit (TSS2_TCTI_CONTEXT *context,
                           size_t             size,
                           uint8_t           *command)
{
    ssize_t write_ret;
    TSS2_RC tss2_ret = TSS2_RC_SUCCESS;

    g_debug ("tss2_tcti_tabrmd_transmit");
    if (context == NULL || command == NULL) {
        return TSS2_TCTI_RC_BAD_REFERENCE;
    }
    if (size == 0) {
        return TSS2_TCTI_RC_BAD_VALUE;
    }
    if (TSS2_TCTI_MAGIC (context) != TSS2_TCTI_TABRMD_MAGIC ||
        TSS2_TCTI_VERSION (context) != TSS2_TCTI_TABRMD_VERSION) {
        return TSS2_TCTI_RC_BAD_CONTEXT;
    }
    if (TSS2_TCTI_TABRMD_STATE (context) != TABRMD_STATE_TRANSMIT) {
        return TSS2_TCTI_RC_BAD_SEQUENCE;
    }
    g_debug_bytes (command, size, 16, 4);
    g_debug ("blocking on FD_TRANSMIT: %d", TSS2_TCTI_TABRMD_FD (context));
    write_ret = write_all (TSS2_TCTI_TABRMD_FD (context),
                           command,
                           size,
                           TSS2_TCTI_TABRMD_IOSTREAM (context));
    /* should switch on possible errors to translate to TSS2 error codes */
    switch (write_ret) {
    case -1:
        g_debug ("tss2_tcti_tabrmd_transmit: error writing to pipe: %s",
                 strerror (errno));
        tss2_ret = TSS2_TCTI_RC_IO_ERROR;
        break;
    case 0:
        g_debug ("tss2_tcti_tabrmd_transmit: EOF returned writing to pipe");
        tss2_ret = TSS2_TCTI_RC_NO_CONNECTION;
        break;
    default:
        if (write_ret == size) {
            TSS2_TCTI_TABRMD_STATE (context) = TABRMD_STATE_RECEIVE;
        } else {
            g_debug ("tss2_tcti_tabrmd_transmit: short write");
            tss2_ret = TSS2_TCTI_RC_GENERAL_FAILURE;
        }
        break;
    }
    return tss2_ret;
}
/*
 * This function maps errno values to TCTI RCs.
 */
static TSS2_RC
errno_to_tcti_rc (int error_number)
{
    switch (error_number) {
    case -1:
        return TSS2_TCTI_RC_NO_CONNECTION;
    case 0:
        return TSS2_RC_SUCCESS;
    case EPROTO:
        return TSS2_TCTI_RC_GENERAL_FAILURE;
    case EAGAIN:
#if EAGAIN != EWOULDBLOCK
    case EWOULDBLOCK:
#endif
        return TSS2_TCTI_RC_TRY_AGAIN;
    case EIO:
        return TSS2_TCTI_RC_IO_ERROR;
    default:
        g_debug ("mapping errno %d with message \"%s\" to "
                 "TSS2_TCTI_RC_GENERAL_FAILURE",
                 error_number, strerror (error_number));
        return TSS2_TCTI_RC_GENERAL_FAILURE;
    }
}
/*
 * This is a thin wrapper around a call to poll. It packages up the provided
 * file descriptor and timeout and polls on that same FD for data or a hangup.
 * Returns:
 *   -1 on timeout
 *   0 when data is ready
 *   errno on error
 */
int
tcti_tabrmd_poll (int        fd,
                  int32_t    timeout)
{
    struct pollfd pollfds [] = {
        {
            .fd = fd,
             .events = POLLIN | POLLPRI | POLLRDHUP,
        }
    };
    int ret;
    int errno_tmp;

    ret = TEMP_FAILURE_RETRY (poll (pollfds,
                                    sizeof (pollfds) / sizeof (struct pollfd),
                                    timeout));
    errno_tmp = errno;
    switch (ret) {
    case -1:
        g_debug ("poll produced error: %d, %s",
                 errno_tmp, strerror (errno_tmp));
        return errno_tmp;
    case 0:
        g_debug ("poll timed out after %" PRId32 " miniseconds", timeout);
        return -1;
    default:
        g_debug ("poll has %d fds ready", ret);
        if (pollfds[0].revents & POLLIN) {
            g_debug ("  POLLIN");
        }
        if (pollfds[0].revents & POLLPRI) {
            g_debug ("  POLLPRI");
        }
        if (pollfds[0].revents & POLLRDHUP) {
            g_debug ("  POLLRDHUP");
        }
        return 0;
    }
}
/*
 * This is the receive function that is exposed to clients through the TCTI
 * API.
 */
TSS2_RC
tss2_tcti_tabrmd_receive (TSS2_TCTI_CONTEXT *context,
                          size_t            *size,
                          uint8_t           *response,
                          int32_t            timeout)
{
    TSS2_TCTI_TABRMD_CONTEXT *tabrmd_ctx = (TSS2_TCTI_TABRMD_CONTEXT*)context;
    size_t ret = 0;

    g_debug ("tss2_tcti_tabrmd_receive");
    if (context == NULL || size == NULL) {
        return TSS2_TCTI_RC_BAD_REFERENCE;
    }
    if (response == NULL && *size != 0) {
        return TSS2_TCTI_RC_BAD_VALUE;
    }
    if (TSS2_TCTI_MAGIC (context) != TSS2_TCTI_TABRMD_MAGIC ||
        TSS2_TCTI_VERSION (context) != TSS2_TCTI_TABRMD_VERSION) {
        return TSS2_TCTI_RC_BAD_CONTEXT;
    }
    if (tabrmd_ctx->state != TABRMD_STATE_RECEIVE) {
        return TSS2_TCTI_RC_BAD_SEQUENCE;
    }
    if (timeout < TSS2_TCTI_TIMEOUT_BLOCK) {
        return TSS2_TCTI_RC_BAD_VALUE;
    }
    if (size == NULL || (response == NULL && *size != 0)) {
        return TSS2_TCTI_RC_BAD_REFERENCE;
    }
    /* response buffer must be at least as large as the header */
    if (response != NULL && *size < TPM_HEADER_SIZE) {
        return TSS2_TCTI_RC_INSUFFICIENT_BUFFER;
    }
    ret = tcti_tabrmd_poll (TSS2_TCTI_TABRMD_FD (context), timeout);
    switch (ret) {
    case -1:
        return TSS2_TCTI_RC_TRY_AGAIN;
    case 0:
        break;
    default:
        return errno_to_tcti_rc (ret);
    }
    /* make sure we've got the response header */
    if (tabrmd_ctx->index < TPM_HEADER_SIZE) {
        ret = read_data (TSS2_TCTI_TABRMD_FD (tabrmd_ctx),
                         &tabrmd_ctx->index,
                         tabrmd_ctx->header_buf,
                         TPM_HEADER_SIZE - tabrmd_ctx->index,
                         tabrmd_ctx->conn);
        if (ret != 0) {
            return errno_to_tcti_rc (ret);
        }
        if (tabrmd_ctx->index == TPM_HEADER_SIZE) {
            tabrmd_ctx->header.tag  = get_response_tag  (tabrmd_ctx->header_buf);
            tabrmd_ctx->header.size = get_response_size (tabrmd_ctx->header_buf);
            tabrmd_ctx->header.code = get_response_code (tabrmd_ctx->header_buf);
            if (tabrmd_ctx->header.size < TPM_HEADER_SIZE) {
                tabrmd_ctx->state = TABRMD_STATE_TRANSMIT;
                return TSS2_TCTI_RC_MALFORMED_RESPONSE;
            }
        }
    }
    /* if response is NULL, caller is querying size, we know size isn't NULL */
    if (response == NULL) {
        *size = tabrmd_ctx->header.size;
        return TSS2_RC_SUCCESS;
    } else if (tabrmd_ctx->index == TPM_HEADER_SIZE) {
        /* once we have the full header copy it to the callers buffer */
        memcpy (response, tabrmd_ctx->header_buf, TPM_HEADER_SIZE);
    }
    if (tabrmd_ctx->header.size == TPM_HEADER_SIZE) {
        tabrmd_ctx->index = 0;
        tabrmd_ctx->state = TABRMD_STATE_TRANSMIT;
        return TSS2_RC_SUCCESS;
    }
    if (*size < tabrmd_ctx->header.size) {
        return TSS2_TCTI_RC_INSUFFICIENT_BUFFER;
    }
    ret = read_data (TSS2_TCTI_TABRMD_FD (tabrmd_ctx),
                     &tabrmd_ctx->index,
                     response,
                     tabrmd_ctx->header.size - tabrmd_ctx->index,
                     TSS2_TCTI_TABRMD_IOSTREAM(tabrmd_ctx));
    if (ret == 0) {
        /* We got all the bytes we asked for, reset the index & state: done */
        *size = tabrmd_ctx->index;
        tabrmd_ctx->index = 0;
        tabrmd_ctx->state = TABRMD_STATE_TRANSMIT;
    }
    return errno_to_tcti_rc (ret);
}

static void
tss2_tcti_tabrmd_finalize (TSS2_TCTI_CONTEXT *context)
{
    GCancellable *cancellable = NULL;
    GError *error = NULL;
    int ret = 0;

    g_debug ("tss2_tcti_tabrmd_finalize");
    if (context == NULL) {
        g_warning ("Invalid parameter");
        return;
    }
    if (TSS2_TCTI_TABRMD_IOSTREAM (context)) {
        if (!g_io_stream_close (TSS2_TCTI_TABRMD_IOSTREAM (context), cancellable, &error)) {
            g_printerr ("Error closing connection: %s\n", error->message);
        }
        TSS2_TCTI_TABRMD_STATE (context) = TABRMD_STATE_FINAL;
        g_object_unref (TSS2_TCTI_TABRMD_IOSTREAM (context));

    } else {
        if (TSS2_TCTI_TABRMD_FD (context) != 0) {
            ret = close (TSS2_TCTI_TABRMD_FD (context));
            TSS2_TCTI_TABRMD_FD (context) = 0;
        }
        if (ret != 0 && ret != EBADF) {
            g_warning ("Failed to close receive pipe: %s", strerror (errno));
        }
        TSS2_TCTI_TABRMD_STATE (context) = TABRMD_STATE_FINAL;
        g_object_unref (TSS2_TCTI_TABRMD_PROXY (context));
    }
}

static TSS2_RC
tss2_tcti_tabrmd_cancel (TSS2_TCTI_CONTEXT *context)
{
    TSS2_RC ret = TSS2_RC_SUCCESS;
    GError *error = NULL;
    gboolean cancel_ret;

    if (context == NULL) {
        return TSS2_TCTI_RC_BAD_CONTEXT;
    }
    g_info("tss2_tcti_tabrmd_cancel: id 0x%" PRIx64,
           TSS2_TCTI_TABRMD_ID (context));
    if (TSS2_TCTI_TABRMD_STATE (context) != TABRMD_STATE_RECEIVE) {
        return TSS2_TCTI_RC_BAD_SEQUENCE;
    }
    cancel_ret = tcti_tabrmd_call_cancel_sync (
                     TSS2_TCTI_TABRMD_PROXY (context),
                     TSS2_TCTI_TABRMD_ID (context),
                     &ret,
                     NULL,
                     &error);
    if (cancel_ret == FALSE) {
        g_warning ("cancel command failed with error code: 0x%" PRIx32
                   ", messag: %s", error->code, error->message);
        ret = error->code;
        g_error_free (error);
    }

    return ret;
}

static TSS2_RC
tss2_tcti_tabrmd_get_poll_handles (TSS2_TCTI_CONTEXT     *context,
                                   TSS2_TCTI_POLL_HANDLE *handles,
                                   size_t                *num_handles)
{
    if (context == NULL) {
        return TSS2_TCTI_RC_BAD_CONTEXT;
    }
    if (num_handles == NULL) {
        return TSS2_TCTI_RC_BAD_REFERENCE;
    }
    if (handles != NULL && *num_handles < 1) {
        return TSS2_TCTI_RC_INSUFFICIENT_BUFFER;
    }
    *num_handles = 1;
    if (handles != NULL) {
        handles [0].fd = TSS2_TCTI_TABRMD_FD (context);
    }
    return TSS2_RC_SUCCESS;
}

static TSS2_RC
tss2_tcti_tabrmd_set_locality (TSS2_TCTI_CONTEXT *context,
                               guint8             locality)
{
    gboolean status;
    TSS2_RC ret = TSS2_RC_SUCCESS;
    GError *error = NULL;

    if (context == NULL) {
        return TSS2_TCTI_RC_BAD_CONTEXT;
    }
    g_info ("tss2_tcti_tabrmd_set_locality: id 0x%" PRIx64,
            TSS2_TCTI_TABRMD_ID (context));
    if (TSS2_TCTI_TABRMD_STATE (context) != TABRMD_STATE_TRANSMIT) {
        return TSS2_TCTI_RC_BAD_SEQUENCE;
    }
    status = tcti_tabrmd_call_set_locality_sync (
                 TSS2_TCTI_TABRMD_PROXY (context),
                 TSS2_TCTI_TABRMD_ID (context),
                 locality,
                 &ret,
                 NULL,
                 &error);

    if (status == FALSE) {
        g_warning ("set locality command failed with error code: 0x%" PRIx32
                   ", message: %s", error->code, error->message);
        ret = error->code;
        g_error_free (error);
    }

    return ret;
}

/*
 * Initialization function to set context data values and function pointers.
 */
void
init_tcti_data (TSS2_TCTI_CONTEXT *context)
{
    TSS2_TCTI_MAGIC (context)            = TSS2_TCTI_TABRMD_MAGIC;
    TSS2_TCTI_VERSION (context)          = TSS2_TCTI_TABRMD_VERSION;
    TSS2_TCTI_TABRMD_STATE (context)     = TABRMD_STATE_TRANSMIT;
    TSS2_TCTI_TRANSMIT (context)         = tss2_tcti_tabrmd_transmit;
    TSS2_TCTI_RECEIVE (context)          = tss2_tcti_tabrmd_receive;
    TSS2_TCTI_FINALIZE (context)         = tss2_tcti_tabrmd_finalize;
    TSS2_TCTI_CANCEL (context)           = tss2_tcti_tabrmd_cancel;
    TSS2_TCTI_GET_POLL_HANDLES (context) = tss2_tcti_tabrmd_get_poll_handles;
    TSS2_TCTI_SET_LOCALITY (context)     = tss2_tcti_tabrmd_set_locality;
}

static gboolean
tcti_tabrmd_call_create_connection_sync_fdlist (TctiTabrmd     *proxy,
                                                GVariant      **out_fds,
                                                guint64        *out_id,
                                                GUnixFDList   **out_fd_list,
                                                GCancellable   *cancellable,
                                                GError        **error)
{
    GVariant *_ret;
    _ret = g_dbus_proxy_call_with_unix_fd_list_sync (G_DBUS_PROXY (proxy),
        "CreateConnection",
        g_variant_new ("()"),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        out_fd_list,
        cancellable,
        error);
    if (_ret == NULL) {
        goto _out;
    }
    g_variant_get (_ret, "(@aht)", out_fds, out_id);
    g_variant_unref (_ret);
_out:
    return _ret != NULL;
}

TSS2_RC
tss2_tcti_tabrmd_init_full (TSS2_TCTI_CONTEXT      *context,
                            size_t                 *size,
                            TCTI_TABRMD_DBUS_TYPE   bus_type,
                            const char             *bus_name)
{
    GBusType g_bus_type;
    GError *error = NULL;
    GVariant *fds_variant;
    guint64 id;
    GUnixFDList *fd_list;
    gboolean call_ret;
    int ret;

    if (context == NULL && size == NULL) {
        return TSS2_TCTI_RC_BAD_VALUE;
    }
    if (context == NULL && size != NULL) {
        *size = sizeof (TSS2_TCTI_TABRMD_CONTEXT);
        return TSS2_RC_SUCCESS;
    }
    if (bus_name == NULL) {
        return TSS2_TCTI_RC_BAD_VALUE;
    }
    switch (bus_type) {
    case TCTI_TABRMD_DBUS_TYPE_SESSION:
        g_bus_type = G_BUS_TYPE_SESSION;
        break;
    case TCTI_TABRMD_DBUS_TYPE_SYSTEM:
        g_bus_type = G_BUS_TYPE_SYSTEM;
        break;
    default:
        return TSS2_TCTI_RC_BAD_VALUE;
    }
    /* Register dbus error mapping for tabrmd. Gets us RCs from Gerror codes */
    TABRMD_ERROR;
    init_tcti_data (context);
    TSS2_TCTI_TABRMD_PROXY (context) =
        tcti_tabrmd_proxy_new_for_bus_sync (
            g_bus_type,
            G_DBUS_PROXY_FLAGS_NONE,
            bus_name,
            TABRMD_DBUS_PATH, /* object */
            NULL,                          /* GCancellable* */
            &error);
    if (TSS2_TCTI_TABRMD_PROXY (context) == NULL) {
        g_error ("failed to allocate dbus proxy object: %s", error->message);
    }
    call_ret = tcti_tabrmd_call_create_connection_sync_fdlist (
        TSS2_TCTI_TABRMD_PROXY (context),
        &fds_variant,
        &id,
        &fd_list,
        NULL,
        &error);
    if (call_ret == FALSE) {
        g_warning ("Failed to create connection with service: %s",
                 error->message);
        return TSS2_TCTI_RC_NO_CONNECTION;
    }
    if (fd_list == NULL) {
        g_error ("call to CreateConnection returned a NULL GUnixFDList");
    }
    gint num_handles = g_unix_fd_list_get_length (fd_list);
    if (num_handles != 1) {
        g_error ("CreateConnection expected to return 1 handles, received %d",
                 num_handles);
    }
    gint fd = g_unix_fd_list_get (fd_list, 0, &error);
    if (fd == -1) {
        g_error ("unable to get receive handle from GUnixFDList: %s",
                 error->message);
    }
    ret = set_flags (fd, O_NONBLOCK);
    if (ret == -1) {
        g_error ("failed to set O_NONBLOCK for client fd: %d", fd);
    }
    TSS2_TCTI_TABRMD_FD (context) = fd;
    TSS2_TCTI_TABRMD_ID (context) = id;
    g_debug ("initialized tabrmd TCTI context with id: 0x%" PRIx64,
             TSS2_TCTI_TABRMD_ID (context));

    return TSS2_RC_SUCCESS;
}

TSS2_RC
tss2_tcti_tabrmd_init (TSS2_TCTI_CONTEXT *context,
                       size_t            *size)
{
    return tss2_tcti_tabrmd_init_full (context,
                                       size,
                                       TCTI_TABRMD_DBUS_TYPE_DEFAULT,
                                       TCTI_TABRMD_DBUS_NAME_DEFAULT);
}

static gboolean
check_server_certificate (GTlsClientConnection *conn,
                        GTlsCertificate      *cert,
                        GTlsCertificateFlags  errors,
                        gpointer              user_data)
{
    g_print ("Certificate would have been rejected ( ");
    if (errors & G_TLS_CERTIFICATE_UNKNOWN_CA)
        g_print ("unknown-ca ");
    if (errors & G_TLS_CERTIFICATE_BAD_IDENTITY)
        g_print ("bad-identity ");
    if (errors & G_TLS_CERTIFICATE_NOT_ACTIVATED)
        g_print ("not-activated ");
    if (errors & G_TLS_CERTIFICATE_EXPIRED)
        g_print ("expired ");
    if (errors & G_TLS_CERTIFICATE_REVOKED)
        g_print ("revoked ");
    if (errors & G_TLS_CERTIFICATE_INSECURE)
        g_print ("insecure ");
    g_print (") but accepting anyway.\n");

    return TRUE;
}

static gboolean
tcti_tabrmd_call_create_connection_tls (const char       *ip_addr,
                                        unsigned int      port,
                                        gboolean          tls_enabled,
                                        GTlsCertificate  *certificate,
                                        GCancellable     *cancellable,
                                        GIOStream       **connection,
                                        GSocket         **socket,
                                        guint64          *id,
                                        GError          **error)
{
    GSocketType socket_type = G_SOCKET_TYPE_STREAM;
    GSocketFamily socket_family;
    GSocketConnectable *connectable;
    GSocketAddressEnumerator *enumerator;
    GIOStream *tls_conn;
    GTlsInteraction *interaction;
    GSocketAddress *src_address, *address = NULL;
    const char *host_and_port = NULL;
    GError *err = NULL;
    int read_timeout = 1;

    /* parse ip addr to get the family of socket */
    socket_family = G_SOCKET_FAMILY_IPV4;

    /* concatenate ip_addr and port */
    if (socket_family == G_SOCKET_FAMILY_IPV4)
        host_and_port = g_strdup_printf ("%s:%d", ip_addr, port);
    else
        host_and_port = g_strdup_printf ("[%s]:%d", ip_addr, port);

    *socket = g_socket_new (socket_family, socket_type, 0, error);
    if (*socket == NULL) {
        return FALSE;
    }

    if (read_timeout)
        g_socket_set_timeout (*socket, read_timeout);

    connectable = g_network_address_parse (host_and_port,
                                           TCTI_TABRMD_TLS_PORT_DEFAULT,
                                           error);
    if (connectable == NULL) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Could not parse '%s' as unix socket name", host_and_port);
        return FALSE;
    }

    enumerator = g_socket_connectable_enumerate (connectable);
    while (TRUE) {
        address = g_socket_address_enumerator_next (enumerator, cancellable, error);
        if (address == NULL) {
            if (error != NULL && *error == NULL)
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "No more addresses to try");
            return FALSE;
        }
        if (g_socket_connect (*socket, address, cancellable, &err))
            break;
        g_message ("Connection to %s failed: %s, trying next\n",
                   socket_address_to_string (address), err->message);
        g_clear_error (&err);
        g_object_unref (address);
    }
    g_object_unref (enumerator);

    g_print ("Connected to %s\n", socket_address_to_string (address));
    g_object_unref (address);
    src_address = g_socket_get_local_address (*socket, error);
    if (!src_address) {
        g_prefix_error (error, "Error getting local address: ");
        return FALSE;
    }
    g_print ("local address: %s\n", socket_address_to_string (src_address));
    *id = g_str_hash (socket_address_to_string (src_address));
    g_object_unref (src_address);
    *connection = G_IO_STREAM (g_socket_connection_factory_create_connection (*socket));

    if (tls_enabled) {
        tls_conn = g_tls_client_connection_new (*connection, connectable, error);
        if (!tls_conn) {
            g_prefix_error (error, "Could not create TLS connection: ");
            return FALSE;
        }

        g_signal_connect (tls_conn, "accept-certificate",
                          G_CALLBACK (check_server_certificate), NULL);

        interaction = g_tls_console_interaction_new ();
        g_tls_connection_set_interaction (G_TLS_CONNECTION (tls_conn), interaction);
        g_object_unref (interaction);

        if (certificate)
            g_tls_connection_set_certificate (G_TLS_CONNECTION (tls_conn), certificate);

        g_object_unref (*connection);
        *connection = G_IO_STREAM (tls_conn);

        if (!g_tls_connection_handshake (G_TLS_CONNECTION (tls_conn),
                                         cancellable,
                                         error)) {
            g_prefix_error (error, "Error during TLS handshake: ");
            return FALSE;
        }
    }
    g_object_unref (connectable);

    return TRUE;
}

TSS2_RC
tss2_tcti_tabrmd_tls_init (TSS2_TCTI_CONTEXT      *context,
                           size_t                 *size,
                           const char             *ip_addr,
                           unsigned int            port,
                           const char             *cert_file,
                           bool                    tls_enabled)
{
    GSocket *socket = NULL;
    GCancellable *cancellable = NULL;
    GIOStream *connection = NULL;
    GTlsCertificate  *certificate = NULL;
    gint fd;
    guint64 id;
    GError *error = NULL;
    gboolean call_ret;

    if (context == NULL && size == NULL) {
        return TSS2_TCTI_RC_BAD_VALUE;
    }
    if (context == NULL && size != NULL) {
        *size = sizeof (TSS2_TCTI_TABRMD_CONTEXT);
        return TSS2_RC_SUCCESS;
    }
    if (ip_addr == NULL) {
        //TODO more checking
        return TSS2_TCTI_RC_BAD_VALUE;
    }
    if (cert_file) {
        certificate = g_tls_certificate_new_from_file (cert_file, &error);
        if (!certificate) {
            g_error ("Could not read certificate '%s': %s",
                     cert_file, error->message);
        }
    }

    init_tcti_data (context);
    call_ret = tcti_tabrmd_call_create_connection_tls(ip_addr,
                                                      port,
                                                      tls_enabled,
                                                      certificate,
                                                      cancellable,
                                                      &connection,
                                                      &socket,
                                                      &id,
                                                      &error);
    if (call_ret == FALSE) {
        g_warning ("Failed to create connection with service: %s",
                   error->message);
        return TSS2_TCTI_RC_NO_CONNECTION;
    }
    if (connection == NULL) {
        g_error ("call to CreateConnection returned a NULL GIOStream");
    }

    /* TODO: Test non-blocking connect/handshake */
    // g_socket_set_blocking (socket, FALSE);

    TSS2_TCTI_TABRMD_IOSTREAM (context) = connection;

    fd = g_socket_get_fd (socket);
    if (fd == -1) {
        g_error ("failed to get handle from socket: %s",
                 error->message);
    }
    TSS2_TCTI_TABRMD_FD (context) = fd;

    TSS2_TCTI_TABRMD_ID (context) = id;
    g_debug ("initialized tabrmd TCTI context with id: 0x%" PRIx64,
             TSS2_TCTI_TABRMD_ID (context));

    //TODO: need to move to finalizaion function
    //g_object_unref (socket);

    return TSS2_RC_SUCCESS;
}
