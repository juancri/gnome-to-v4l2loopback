#include "portal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static void on_request_response(GDBusConnection *connection,
                               const gchar *sender_name,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *signal_name,
                               GVariant *parameters,
                               gpointer user_data);

static void on_session_closed(GDBusConnection *connection,
                             const gchar *sender_name,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *signal_name,
                             GVariant *parameters,
                             gpointer user_data);

PortalSession* portal_session_new(void) {
    PortalSession *session = g_malloc0(sizeof(PortalSession));

    session->main_loop = g_main_loop_new(NULL, FALSE);
    session->pipewire_fd = -1;
    session->session_active = false;
    session->session_closed_callback = NULL;
    session->session_closed_user_data = NULL;

    // Connect to session bus
    GError *error = NULL;
    session->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error) {
        fprintf(stderr, "Failed to connect to session bus: %s\n", error->message);
        g_error_free(error);
        portal_session_free(session);
        return NULL;
    }

    // Create proxy for the portal
    session->portal_proxy = g_dbus_proxy_new_sync(
        session->connection,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        PORTAL_BUS_NAME,
        PORTAL_OBJECT_PATH,
        PORTAL_SCREENCAST_INTERFACE,
        NULL,
        &error);

    if (error) {
        fprintf(stderr, "Failed to create portal proxy: %s\n", error->message);
        g_error_free(error);
        portal_session_free(session);
        return NULL;
    }

    return session;
}

void portal_session_free(PortalSession *session) {
    if (!session) return;

    if (session->pipewire_fd >= 0) {
        close(session->pipewire_fd);
    }

    g_free(session->session_handle);
    g_free(session->request_token);

    if (session->portal_proxy) {
        g_object_unref(session->portal_proxy);
    }

    if (session->connection) {
        g_object_unref(session->connection);
    }

    if (session->main_loop) {
        g_main_loop_unref(session->main_loop);
    }

    g_free(session);
}

char* portal_generate_token(void) {
    static uint32_t counter = 0;
    counter++;

    return g_strdup_printf("gnome_to_v4l2_%u_%u", (uint32_t)time(NULL), counter);
}

static char* sanitize_unique_name(const char *unique_name) {
    // Skip the ':' prefix and replace dots with underscores
    const char *name = unique_name + 1;
    char *sanitized = g_strdup(name);
    for (char *p = sanitized; *p; p++) {
        if (*p == '.') *p = '_';
    }
    return sanitized;
}

bool portal_create_session(PortalSession *session, PortalSessionCallback callback, void *user_data) {
    if (!session || !session->portal_proxy) {
        return false;
    }

    // Generate unique tokens
    char *handle_token = portal_generate_token();
    char *session_handle_token = portal_generate_token();

    // Store request token for callback matching
    g_free(session->request_token);
    session->request_token = g_strdup(handle_token);

    // Build options
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(handle_token));
    g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_handle_token));

    // Expected session handle path
    char *sanitized_name = sanitize_unique_name(g_dbus_connection_get_unique_name(session->connection));
    g_free(session->session_handle);
    session->session_handle = g_strdup_printf("/org/freedesktop/portal/desktop/session/%s/%s",
                                            sanitized_name, session_handle_token);
    g_free(sanitized_name);

    // Subscribe to request response signal
    char *sanitized_name_req = sanitize_unique_name(g_dbus_connection_get_unique_name(session->connection));
    char *request_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                                       sanitized_name_req, handle_token);
    g_free(sanitized_name_req);

    g_dbus_connection_signal_subscribe(
        session->connection,
        PORTAL_BUS_NAME,
        PORTAL_REQUEST_INTERFACE,
        "Response",
        request_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_request_response,
        session,
        NULL);

    // Subscribe to session closed signal
    g_dbus_connection_signal_subscribe(
        session->connection,
        PORTAL_BUS_NAME,
        PORTAL_SESSION_INTERFACE,
        "Closed",
        session->session_handle,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_session_closed,
        session,
        NULL);

    // Store callback for later use
    g_object_set_data(G_OBJECT(session->portal_proxy), "create_session_callback", callback);
    g_object_set_data(G_OBJECT(session->portal_proxy), "create_session_user_data", user_data);

    // Call CreateSession
    printf("DEBUG: Calling CreateSession...\n");
    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_sync(
        session->portal_proxy,
        "CreateSession",
        g_variant_new("(a{sv})", &builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    g_free(handle_token);
    g_free(session_handle_token);
    g_free(request_path);

    if (error) {
        fprintf(stderr, "Failed to create session: %s\n", error->message);
        g_error_free(error);
        return false;
    }

    if (result) {
        g_variant_unref(result);
    }

    return true;
}

bool portal_select_sources(PortalSession *session, PortalSessionCallback callback, void *user_data) {
    if (!session || !session->portal_proxy || !session->session_handle) {
        return false;
    }

    char *handle_token = portal_generate_token();

    // Store request token for callback matching
    g_free(session->request_token);
    session->request_token = g_strdup(handle_token);

    // Build options
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(handle_token));
    g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(1)); // MONITOR = 1
    g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(2)); // EMBEDDED = 2

    // Subscribe to request response signal
    char *sanitized_name_req = sanitize_unique_name(g_dbus_connection_get_unique_name(session->connection));
    char *request_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                                       sanitized_name_req, handle_token);
    g_free(sanitized_name_req);

    g_dbus_connection_signal_subscribe(
        session->connection,
        PORTAL_BUS_NAME,
        PORTAL_REQUEST_INTERFACE,
        "Response",
        request_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_request_response,
        session,
        NULL);

    // Store callback for later use
    g_object_set_data(G_OBJECT(session->portal_proxy), "select_sources_callback", callback);
    g_object_set_data(G_OBJECT(session->portal_proxy), "select_sources_user_data", user_data);

    // Call SelectSources
    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_sync(
        session->portal_proxy,
        "SelectSources",
        g_variant_new("(oa{sv})", session->session_handle, &builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    g_free(handle_token);
    g_free(request_path);

    if (error) {
        fprintf(stderr, "Failed to select sources: %s\n", error->message);
        g_error_free(error);
        return false;
    }

    if (result) {
        g_variant_unref(result);
    }

    return true;
}

bool portal_start_session(PortalSession *session, PortalNodeCallback callback, void *user_data) {
    if (!session || !session->portal_proxy || !session->session_handle) {
        return false;
    }

    char *handle_token = portal_generate_token();

    // Store request token for callback matching
    g_free(session->request_token);
    session->request_token = g_strdup(handle_token);

    // Build options
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(handle_token));

    // Subscribe to request response signal
    char *sanitized_name_req = sanitize_unique_name(g_dbus_connection_get_unique_name(session->connection));
    char *request_path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s",
                                       sanitized_name_req, handle_token);
    g_free(sanitized_name_req);

    g_dbus_connection_signal_subscribe(
        session->connection,
        PORTAL_BUS_NAME,
        PORTAL_REQUEST_INTERFACE,
        "Response",
        request_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_request_response,
        session,
        NULL);

    // Store callback for later use
    g_object_set_data(G_OBJECT(session->portal_proxy), "start_session_callback", callback);
    g_object_set_data(G_OBJECT(session->portal_proxy), "start_session_user_data", user_data);

    // Call Start
    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_sync(
        session->portal_proxy,
        "Start",
        g_variant_new("(osa{sv})", session->session_handle, "", &builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error);

    g_free(handle_token);
    g_free(request_path);

    if (error) {
        fprintf(stderr, "Failed to start session: %s\n", error->message);
        g_error_free(error);
        return false;
    }

    if (result) {
        g_variant_unref(result);
    }

    return true;
}

bool portal_open_pipewire_remote(PortalSession *session, PortalNodeCallback callback, void *user_data) {
    if (!session || !session->portal_proxy || !session->session_handle) {
        return false;
    }

    // Build options
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

    // Store callback for later use
    g_object_set_data(G_OBJECT(session->portal_proxy), "open_pipewire_callback", callback);
    g_object_set_data(G_OBJECT(session->portal_proxy), "open_pipewire_user_data", user_data);

    // Call OpenPipeWireRemote
    GError *error = NULL;
    GUnixFDList *fd_list = NULL;
    GVariant *result = g_dbus_proxy_call_with_unix_fd_list_sync(
        session->portal_proxy,
        "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", session->session_handle, &builder),
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &fd_list,
        NULL,
        &error);

    if (error) {
        fprintf(stderr, "Failed to open PipeWire remote: %s\n", error->message);
        g_error_free(error);
        return false;
    }

    if (result && fd_list) {
        gint32 fd_index;
        g_variant_get(result, "(h)", &fd_index);

        session->pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, &error);
        if (error) {
            fprintf(stderr, "Failed to get file descriptor: %s\n", error->message);
            g_error_free(error);
            g_variant_unref(result);
            g_object_unref(fd_list);
            return false;
        }

        // Call the callback with the node ID and file descriptor
        if (callback) {
            callback(session, session->node_id, session->pipewire_fd, user_data);
        }

        g_variant_unref(result);
        g_object_unref(fd_list);
        return true;
    }

    return false;
}

void portal_run_main_loop(PortalSession *session) {
    if (session && session->main_loop) {
        g_main_loop_run(session->main_loop);
    }
}

void portal_quit_main_loop(PortalSession *session) {
    if (session && session->main_loop && g_main_loop_is_running(session->main_loop)) {
        g_main_loop_quit(session->main_loop);
    }
}

static void on_request_response(GDBusConnection *connection,
                               const gchar *sender_name,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *signal_name,
                               GVariant *parameters,
                               gpointer user_data) {
    PortalSession *session = (PortalSession*)user_data;

    printf("DEBUG: Received portal response signal\n");

    guint32 response;
    GVariant *results;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    printf("DEBUG: Portal response code: %u\n", response);

    if (response != 0) {
        fprintf(stderr, "Portal request failed with response: %u\n", response);
        portal_quit_main_loop(session);
        g_variant_unref(results);
        return;
    }

    // Check if this is a Start response (contains streams)
    GVariant *streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (streams) {
        // Extract node ID from first stream
        GVariantIter iter;
        g_variant_iter_init(&iter, streams);

        GVariant *stream_data;
        if (g_variant_iter_next(&iter, "@(ua{sv})", &stream_data)) {
            guint32 node_id;
            GVariant *stream_properties;
            g_variant_get(stream_data, "(u@a{sv})", &node_id, &stream_properties);

            session->node_id = node_id;
            session->session_active = true;

            printf("Screen capture session started. PipeWire node ID: %u\n", node_id);

            // Call the start session callback
            PortalNodeCallback callback = g_object_get_data(G_OBJECT(session->portal_proxy), "start_session_callback");
            void *callback_user_data = g_object_get_data(G_OBJECT(session->portal_proxy), "start_session_user_data");

            if (callback) {
                callback(session, node_id, session->pipewire_fd, callback_user_data);
            }

            g_variant_unref(stream_properties);
            g_variant_unref(stream_data);
        }

        g_variant_unref(streams);
    } else {
        // Handle other responses (CreateSession, SelectSources)
        printf("Portal operation completed successfully\n");

        // Call appropriate callback based on which operation this was
        PortalSessionCallback create_callback = g_object_get_data(G_OBJECT(session->portal_proxy), "create_session_callback");
        PortalSessionCallback select_callback = g_object_get_data(G_OBJECT(session->portal_proxy), "select_sources_callback");

        if (create_callback) {
            void *callback_user_data = g_object_get_data(G_OBJECT(session->portal_proxy), "create_session_user_data");
            create_callback(session, true, callback_user_data);
            g_object_set_data(G_OBJECT(session->portal_proxy), "create_session_callback", NULL);
        } else if (select_callback) {
            void *callback_user_data = g_object_get_data(G_OBJECT(session->portal_proxy), "select_sources_user_data");
            select_callback(session, true, callback_user_data);
            g_object_set_data(G_OBJECT(session->portal_proxy), "select_sources_callback", NULL);
        }
    }

    g_variant_unref(results);
}

static void on_session_closed(GDBusConnection *connection,
                             const gchar *sender_name,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *signal_name,
                             GVariant *parameters,
                             gpointer user_data) {
    PortalSession *session = (PortalSession*)user_data;

    printf("Portal session closed from GNOME UI\n");
    session->session_active = false;

    // Invoke the session closed callback if set
    if (session->session_closed_callback) {
        session->session_closed_callback(session, session->session_closed_user_data);
    }

    portal_quit_main_loop(session);
}

void portal_set_session_closed_callback(PortalSession *session, PortalSessionClosedCallback callback, void *user_data) {
    if (session) {
        session->session_closed_callback = callback;
        session->session_closed_user_data = user_data;
    }
}