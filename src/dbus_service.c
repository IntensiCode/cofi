#include "dbus_service.h"
#include "log.h"
#include "command_mode.h"
#include <string.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

// Global app data pointer (same pattern as instance.c)
static AppData *g_app_data = NULL;

// Forward declarations
static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data);
static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data);
static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data);
static void handle_method_call(GDBusConnection *connection, const gchar *sender,
                              const gchar *object_path, const gchar *interface_name,
                              const gchar *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation, gpointer user_data);

// Forward declarations from other modules
extern void setup_application(AppData *app, int alignment);
extern void filter_windows(AppData *app, const char *filter);
extern void update_display(AppData *app);
extern void reset_selection(AppData *app);
extern void show_window(AppData *app);
extern void enter_command_mode(AppData *app);

// Deferred command mode entry callback (replicated from instance.c)
static gboolean enter_command_mode_delayed(gpointer data) {
    AppData *app = (AppData*)data;
    if (app) {
        app->command_mode.close_on_exit = TRUE; // Set flag to close window on exit
        enter_command_mode(app);
    }
    return FALSE; // Remove timeout
}

// D-Bus introspection data
static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.cofi.WindowManager'>"
  "    <method name='ShowWindow'>"
  "      <arg type='s' name='mode' direction='in'/>"
  "      <arg type='b' name='success' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

// D-Bus interface vtable
static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    NULL, // get_property
    NULL  // set_property
};

// Convert ShowMode enum to string
const char* show_mode_to_string(ShowMode mode) {
    switch (mode) {
        case SHOW_MODE_WINDOWS:
            return "windows";
        case SHOW_MODE_WORKSPACES:
            return "workspaces";
        case SHOW_MODE_HARPOON:
            return "harpoon";
        case SHOW_MODE_COMMAND:
            return "command";
        default:
            return "windows"; // fallback
    }
}

// Convert string to ShowMode enum
ShowMode string_to_show_mode(const char *mode_str) {
    if (!mode_str) return SHOW_MODE_WINDOWS;
    
    if (strcmp(mode_str, "workspaces") == 0) {
        return SHOW_MODE_WORKSPACES;
    } else if (strcmp(mode_str, "harpoon") == 0) {
        return SHOW_MODE_HARPOON;
    } else if (strcmp(mode_str, "command") == 0) {
        return SHOW_MODE_COMMAND;
    } else {
        return SHOW_MODE_WINDOWS; // default/fallback
    }
}

// Show existing window function
static gboolean show_window_idle(gpointer data) {
    (void)data; // Unused parameter

    if (g_app_data && g_app_data->window) {
        // Show the existing window
        show_window(g_app_data);

        // Enter command mode if requested
        if (g_app_data->start_in_command_mode) {
            g_app_data->start_in_command_mode = 0; // Reset flag
            // Defer entering command mode to ensure proper focus handling
            g_timeout_add(100, enter_command_mode_delayed, g_app_data);
            log_debug("Scheduled command mode entry via D-Bus");
        }

        log_debug("Window shown by D-Bus call from another instance");
    }

    return FALSE; // Remove from idle queue
}


// D-Bus method call handler
static void handle_method_call(GDBusConnection *connection, const gchar *sender,
                              const gchar *object_path, const gchar *interface_name,
                              const gchar *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    
    DBusService *service = (DBusService *)user_data;
    
    if (g_strcmp0(method_name, "ShowWindow") == 0) {
        const gchar *mode_str;
        g_variant_get(parameters, "(&s)", &mode_str);
        
        log_debug("D-Bus ShowWindow called with mode: %s", mode_str);
        
        if (g_app_data) {
            // Convert mode string to ShowMode and set app state
            ShowMode mode = string_to_show_mode(mode_str);

            // Set the appropriate tab/mode based on D-Bus call
            switch (mode) {
                case SHOW_MODE_WORKSPACES:
                    g_app_data->current_tab = TAB_WORKSPACES;
                    g_app_data->start_in_command_mode = 0;
                    break;
                case SHOW_MODE_HARPOON:
                    g_app_data->current_tab = TAB_HARPOON;
                    g_app_data->start_in_command_mode = 0;
                    break;
                case SHOW_MODE_COMMAND:
                    g_app_data->current_tab = TAB_WINDOWS;
                    g_app_data->start_in_command_mode = 1;
                    break;
                case SHOW_MODE_WINDOWS:
                default:
                    g_app_data->current_tab = TAB_WINDOWS;
                    g_app_data->start_in_command_mode = 0;
                    break;
            }

            // Defer window showing to the GTK main loop
            g_idle_add(show_window_idle, NULL);

            log_debug("Window show scheduled via D-Bus call");

            // Return success
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
        } else {
            log_error("D-Bus service has no global app_data");
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        }
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                            G_DBUS_ERROR_UNKNOWN_METHOD,
                                            "Unknown method: %s", method_name);
    }
}

// Bus acquired callback
static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)name;
    
    DBusService *service = (DBusService *)user_data;
    GError *error = NULL;
    
    // Parse introspection data
    GDBusNodeInfo *introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!introspection_data) {
        log_error("Failed to parse D-Bus introspection XML: %s", error->message);
        g_error_free(error);
        return;
    }
    
    // Register object
    service->registration_id = g_dbus_connection_register_object(
        connection,
        COFI_DBUS_OBJECT_PATH,
        introspection_data->interfaces[0],
        &interface_vtable,
        service,  // user_data
        NULL,     // user_data_free_func
        &error
    );
    
    g_dbus_node_info_unref(introspection_data);
    
    if (service->registration_id == 0) {
        log_error("Failed to register D-Bus object: %s", error->message);
        g_error_free(error);
        return;
    }
    
    service->connection = connection;
    log_debug("D-Bus object registered successfully");
}

// Name acquired callback
static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)connection;
    
    DBusService *service = (DBusService *)user_data;
    service->service_registered = TRUE;
    log_info("D-Bus service name acquired: %s", name);
}

// Name lost callback
static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)connection;
    (void)user_data;
    
    log_warn("D-Bus service name lost: %s", name);
}

// Set global app data pointer
void dbus_service_set_app_data(AppData *app_data) {
    g_app_data = app_data;
}

// Initialize D-Bus service
DBusService* dbus_service_new(AppData *app_data) {
    DBusService *service = g_malloc0(sizeof(DBusService));
    if (!service) {
        log_error("Failed to allocate memory for DBusService");
        return NULL;
    }

    // Set global app data pointer
    g_app_data = app_data;
    service->app_data = app_data;
    service->service_registered = FALSE;
    
    // Request service name
    service->name_owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        COFI_DBUS_SERVICE_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired,
        on_name_acquired,
        on_name_lost,
        service,
        NULL
    );
    
    if (service->name_owner_id == 0) {
        log_error("Failed to request D-Bus service name");
        g_free(service);
        return NULL;
    }
    
    log_debug("D-Bus service initialization started");
    return service;
}

// Check if existing instance exists and call ShowWindow
bool dbus_service_check_existing_and_show(const char *mode) {
    GError *error = NULL;
    
    // Get session bus connection
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!connection) {
        log_debug("Failed to get D-Bus connection: %s", error->message);
        g_error_free(error);
        return false;
    }
    
    // Try to call ShowWindow method on existing service
    GVariant *result = g_dbus_connection_call_sync(
        connection,
        COFI_DBUS_SERVICE_NAME,
        COFI_DBUS_OBJECT_PATH,
        COFI_DBUS_INTERFACE_NAME,
        "ShowWindow",
        g_variant_new("(s)", mode),
        G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE,
        150, // 150ms timeout
        NULL,
        &error
    );
    
    g_object_unref(connection);
    
    if (result) {
        gboolean success;
        g_variant_get(result, "(b)", &success);
        g_variant_unref(result);
        
        if (success) {
            log_info("Successfully called ShowWindow(%s) on existing instance", mode);
            return true;
        } else {
            log_warn("ShowWindow call returned false");
            return false;
        }
    } else {
        // Service doesn't exist or call failed
        if (error) {
            log_debug("D-Bus call failed (no existing instance): %s", error->message);
            g_error_free(error);
        }
        return false;
    }
}

// Cleanup D-Bus service
void dbus_service_cleanup(DBusService *service) {
    if (!service) return;
    
    if (service->registration_id > 0 && service->connection) {
        g_dbus_connection_unregister_object(service->connection, service->registration_id);
        service->registration_id = 0;
    }
    
    if (service->name_owner_id > 0) {
        g_bus_unown_name(service->name_owner_id);
        service->name_owner_id = 0;
    }
    
    g_free(service);
    log_debug("D-Bus service cleaned up");
}
