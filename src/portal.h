#ifndef PORTAL_H
#define PORTAL_H

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <stdint.h>
#include <stdbool.h>

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST_INTERFACE "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST_INTERFACE "org.freedesktop.portal.Request"
#define PORTAL_SESSION_INTERFACE "org.freedesktop.portal.Session"

// Forward declaration for callback types
typedef struct PortalSession PortalSession;
typedef void (*PortalSessionCallback)(PortalSession *session, bool success, void *user_data);
typedef void (*PortalNodeCallback)(PortalSession *session, uint32_t node_id, int pipewire_fd, void *user_data);
typedef void (*PortalSessionClosedCallback)(PortalSession *session, void *user_data);

struct PortalSession {
    GDBusConnection *connection;
    GDBusProxy *portal_proxy;
    char *session_handle;
    char *request_token;
    uint32_t node_id;
    int pipewire_fd;
    bool session_active;
    GMainLoop *main_loop;
    PortalSessionClosedCallback session_closed_callback;
    void *session_closed_user_data;
};

typedef struct {
    uint32_t width;
    uint32_t height;
} PortalDimensions;

// Core portal functions
PortalSession* portal_session_new(void);
void portal_session_free(PortalSession *session);

// Portal workflow functions
bool portal_create_session(PortalSession *session, PortalSessionCallback callback, void *user_data);
bool portal_select_sources(PortalSession *session, PortalSessionCallback callback, void *user_data);
bool portal_start_session(PortalSession *session, PortalNodeCallback callback, void *user_data);
bool portal_open_pipewire_remote(PortalSession *session, PortalNodeCallback callback, void *user_data);

// Utility functions
char* portal_generate_token(void);
void portal_run_main_loop(PortalSession *session);
void portal_quit_main_loop(PortalSession *session);

// Session event handling
void portal_set_session_closed_callback(PortalSession *session, PortalSessionClosedCallback callback, void *user_data);

#endif // PORTAL_H