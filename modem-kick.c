/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2024 JUCR GmbH
 */

#include <glib.h>
#include <glib-unix.h>
#include <libmm-glib.h>

#ifdef DEBUG
#define RESET_POLL_SECONDS 15
#define KICK_INTERVAL_SECONDS 60  /* 1 minute */
#else
#define RESET_POLL_SECONDS 300
#define KICK_INTERVAL_SECONDS 605  /* 10 minutes + 5 seconds */
#endif

typedef struct ModemContext ModemContext;

typedef struct {
    GDBusConnection *connection;
    GMainLoop       *loop;
    GCancellable    *cancellable;
    MMManager       *mm;

    GHashTable *modems;

    guint name_owner_changed_id;
    guint object_added_id;
    guint object_removed_id;

    guint reset_poll_id;
} Context;

static Context *
context_new (void)
{
    Context *ctx;

    ctx = g_slice_new0 (Context);
    ctx->loop = g_main_loop_new (NULL, FALSE);
    ctx->cancellable = g_cancellable_new ();
    ctx->modems = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_object_unref);
    return ctx;
}

static void
context_clear_modems (Context *ctx)
{
    g_message ("clearing modems");
    g_hash_table_remove_all (ctx->modems);
}

static void
context_clear_manager (Context *ctx)
{
    if (ctx->name_owner_changed_id) {
        g_signal_handler_disconnect (ctx->mm, ctx->name_owner_changed_id);
        ctx->name_owner_changed_id = 0;
    }
    if (ctx->object_added_id) {
        g_signal_handler_disconnect (ctx->mm, ctx->object_added_id);
        ctx->object_added_id = 0;
    }
    if (ctx->object_removed_id) {
        g_signal_handler_disconnect (ctx->mm, ctx->object_removed_id);
        ctx->object_removed_id = 0;
    }

    g_clear_object (&ctx->mm);

    context_clear_modems (ctx);
}

static void
context_free (Context *ctx)
{
    context_clear_manager (ctx);

    if (ctx->reset_poll_id) {
        g_source_remove (ctx->reset_poll_id);
        ctx->reset_poll_id = 0;
    }

    g_hash_table_destroy (ctx->modems);
    g_cancellable_cancel (ctx->cancellable);
    g_clear_object (&ctx->cancellable);
    g_clear_object (&ctx->connection);
    g_main_loop_unref (ctx->loop);
    g_slice_free (Context, ctx);
}

/*****************************************************************************/

static void modem_registration_changed (MMModem3gpp *modem_3gpp, GParamSpec *pspec, MMObject *modem_object);

typedef enum {
    MODEM_OP_STATE_NONE = 0,
    MODEM_OP_STATE_DISABLE,
    MODEM_OP_STATE_LOW_POWER,
    MODEM_OP_STATE_ENABLE,
    MODEM_OP_STATE_FINISH,
} OpState;

struct ModemContext {
    const gchar *path;
    MMModem     *modem;
    MMModem3gpp *modem_3gpp;

    guint reg_state_changed_id;

    /* monotonic timestamp when modem was last idle/denied; set to 0 when modem
     * enters a registration state other than idle/denied.
     */
    gint64 timestamp;

    /* Created & used each time modem needs a kick */
    GCancellable *cancellable;

    OpState op_state;
    guint   next_id;
    guint   tries;
};

static ModemContext *
get_modem_context (MMObject *obj)
{
    return g_object_get_data (G_OBJECT (obj), "modem-context");
}

static ModemContext *
modem_context_new (MMObject *modem_object, MMModem *modem, MMModem3gpp *modem_3gpp)
{
    ModemContext *modem_ctx;

    modem_ctx = g_slice_new0 (ModemContext);
    modem_ctx->path = mm_object_get_path (modem_object);
    modem_ctx->modem = modem;
    modem_ctx->modem_3gpp = modem_3gpp;

    return modem_ctx;
}

static void
modem_context_cancel_op (ModemContext *modem_ctx)
{
    modem_ctx->op_state = MODEM_OP_STATE_NONE;
    g_cancellable_cancel (modem_ctx->cancellable);
    g_clear_object (&modem_ctx->cancellable);
    if (modem_ctx->next_id)
        g_source_remove (modem_ctx->next_id);
    modem_ctx->next_id = 0;
    modem_ctx->tries = 0;
}

static void
modem_context_free (ModemContext *modem_ctx)
{
    modem_context_cancel_op (modem_ctx);
    g_slice_free (ModemContext, modem_ctx);
}

static void
modem_registration_changed (MMModem3gpp *modem_3gpp, GParamSpec *pspec, MMObject *modem_object)
{
    ModemContext                 *modem_ctx = get_modem_context (modem_object);
    MMModem3gppRegistrationState  reg_state;

    reg_state = mm_modem_3gpp_get_registration_state (modem_3gpp);
    g_message ("%s: registration changed to %s", modem_ctx->path, mm_modem_3gpp_registration_state_get_string (reg_state));
    if (reg_state == MM_MODEM_3GPP_REGISTRATION_STATE_IDLE || reg_state == MM_MODEM_3GPP_REGISTRATION_STATE_DENIED) {
        if (modem_ctx->timestamp == 0) {
            modem_ctx->timestamp = g_get_monotonic_time ();
            g_message ("%s: save idle/denied timestamp %" G_GINT64_FORMAT, modem_ctx->path, modem_ctx->timestamp);
        }
    } else {
        g_message ("%s: registered; clearing idle/denied timestamp", modem_ctx->path);
        modem_ctx->timestamp = 0;
    }
}

/*****************************************************************************/

static void ensure_manager (Context *ctx);

static void
handle_object_added (MMManager *mm, MMObject *modem_object, Context *ctx)
{
    const gchar  *path;
    MMModem      *modem_iface = NULL;
    MMModem3gpp  *modem_3gpp_iface = NULL;
    ModemContext *modem_ctx;

    path = mm_object_get_path (modem_object);

    /* Ensure we have the 'Modem' interface at least */
    modem_iface = mm_object_peek_modem (modem_object);
    if (!modem_iface) {
        g_warning ("Error: modem %s had no modem interface", path);
        return;
    }

    /* Ensure we have a primary port reported */
    if (!mm_modem_get_primary_port (modem_iface)) {
        g_warning ("Error: modem %s had no primary port", path);
        return;
    }

    modem_3gpp_iface = mm_object_peek_modem_3gpp (modem_object);
    if (!modem_3gpp_iface) {
        g_message ("Ignoring non-3GPP modem %s", path);
        return;
    }

    g_message ("%s: added", path);
    modem_ctx = modem_context_new (modem_object, modem_iface, modem_3gpp_iface);
    g_object_set_data_full (G_OBJECT (modem_object), "modem-context", modem_ctx, (GDestroyNotify) modem_context_free);

    modem_ctx->reg_state_changed_id = g_signal_connect (modem_3gpp_iface,
                                                        "notify::registration-state",
                                                        G_CALLBACK (modem_registration_changed),
                                                        modem_object);
    modem_registration_changed (modem_3gpp_iface, NULL, modem_object);

    g_hash_table_insert (ctx->modems, g_strdup (path), g_object_ref (modem_object));
}

static void
handle_object_removed (MMManager *manager, MMObject *modem_object, Context *ctx)
{
    const gchar *path = mm_object_get_path (modem_object);

    g_message ("%s: removed", path);
    g_hash_table_remove (ctx->modems, path);
}

static void
mm_available (Context *ctx)
{
    GList *modems, *l;

    /* Get initial modems */
    modems = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER(ctx->mm));
    for (l = modems; l; l = g_list_next(l)) {
        handle_object_added (ctx->mm, MM_OBJECT (l->data), ctx);
    }
    g_list_free_full (modems, (GDestroyNotify) g_object_unref);
}

static void
check_name_owner (Context *ctx)
{
    g_autofree gchar *name_owner = NULL;

    name_owner = g_dbus_object_manager_client_get_name_owner (G_DBUS_OBJECT_MANAGER_CLIENT (ctx->mm));
    if (name_owner) {
        g_message ("ModemManager is running");
        mm_available (ctx);
    } else {
        g_message ("ModemManager is not running");
    }
}

static void
handle_name_owner_changed (MMManager *modem_manager, GParamSpec *pspec, Context *ctx)
{
    g_autofree gchar *name_owner = NULL;

    name_owner = g_dbus_object_manager_client_get_name_owner (G_DBUS_OBJECT_MANAGER_CLIENT (ctx->mm));
    if (!name_owner) {
        g_message ("ModemManager no longer running");
        context_clear_modems (ctx);
    } else {
        g_message ("ModemManager now running");

        /* Hack: GDBusObjectManagerClient won't signal object events if it was
         * created while MM was not on the bus. Work around that by recreating the
         * manager when MM shows up. Or get a fixed GIO.
         */
        context_clear_manager (ctx);
        ensure_manager (ctx);
    }
}

static void
modem_manager_new_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
    Context           *ctx = user_data;
    g_autoptr(GError)  error = NULL;

    ctx->mm = mm_manager_new_finish (res, &error);
    if (!ctx->mm) {
        g_warning ("Error: failed to connect to ModemManager: %s", error->message);
        return;
    }
    g_message ("Watching D-Bus for ModemManager...");

    /* Setup signals in the GDBusObjectManagerClient */
    ctx->name_owner_changed_id = g_signal_connect (ctx->mm,
                                                   "notify::name-owner",
                                                   G_CALLBACK (handle_name_owner_changed),
                                                   ctx);
    ctx->object_added_id = g_signal_connect (ctx->mm,
                                             "object-added",
                                             G_CALLBACK (handle_object_added),
                                             ctx);
    ctx->object_removed_id = g_signal_connect (ctx->mm,
                                               "object-removed",
                                               G_CALLBACK (handle_object_removed),
                                               ctx);

    check_name_owner (ctx);
}

static void
ensure_manager (Context *ctx)
{
    mm_manager_new (ctx->connection,
                    G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
                    ctx->cancellable,
                    modem_manager_new_cb,
                    ctx);
}

static void
bus_get_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
    Context           *ctx = user_data;
    g_autoptr(GError)  error = NULL;
    GDBusConnection   *connection;

    connection = g_bus_get_finish (res, &error);
    if (!connection) {
        g_warning ("Error: failed to connect to D-Bus: %s", error->message);
        g_main_loop_quit (ctx->loop);
        return;
    }
    ctx->connection = connection;

    ensure_manager (ctx);
}

static gboolean modem_op_state_run (MMObject *modem_object);
static void modem_schedule_op_state (MMObject *modem_object, OpState new_state);

static void
modem_schedule_retry_op_state (MMObject *modem_object)
{
    ModemContext *modem_ctx = get_modem_context (modem_object);

    modem_ctx->tries++;
    if (modem_ctx->tries > 3) {
        g_message ("%s: too many retries; failing operation", modem_ctx->path);
        modem_schedule_op_state (modem_object, MODEM_OP_STATE_FINISH);
    } else {
        /* retry same op state */
        modem_schedule_op_state (modem_object, modem_ctx->op_state);
    }
}

static void
modem_schedule_op_state (MMObject *modem_object, OpState new_state)
{
    ModemContext *modem_ctx = get_modem_context (modem_object);

    modem_ctx->op_state = new_state;
    g_assert (modem_ctx->next_id == 0);
    modem_ctx->next_id = g_timeout_add_seconds (10, (GSourceFunc) modem_op_state_run, modem_object);
}

static void
modem_enable_ready (MMModem *modem_iface, GAsyncResult *res, MMObject *modem_object)
{
    const gchar       *path = mm_object_get_path (modem_object);
    g_autoptr(GError) error = NULL;

    if (!mm_modem_enable_finish(modem_iface, res, &error)) {
        g_warning ("Error: %s failed to enable: '%s'", path, error->message);
        modem_schedule_retry_op_state (modem_object);
    } else {
        modem_schedule_op_state (modem_object, MODEM_OP_STATE_FINISH);
    }

    g_object_unref (modem_object);
}

static void
set_power_state_low_ready (MMModem *modem, GAsyncResult *result, MMObject *modem_object)
{
    ModemContext      *modem_ctx = get_modem_context (modem_object);
    g_autoptr(GError) error = NULL;

    if (!mm_modem_set_power_state_finish (modem, result, &error)) {
        g_warning ("Error: %s failed to set low-power: '%s'", modem_ctx->path, error->message);
        modem_schedule_retry_op_state (modem_object);
    } else {
        modem_schedule_op_state (modem_object, MODEM_OP_STATE_ENABLE);
    }

    g_object_unref (modem_object);
}

static void
modem_disable_ready (MMModem *modem_iface, GAsyncResult *res, MMObject *modem_object)
{
    ModemContext      *modem_ctx = get_modem_context (modem_object);
    OpState            next_state = MODEM_OP_STATE_LOW_POWER;
    g_autoptr(GError)  error = NULL;

    if (!mm_modem_disable_finish (modem_iface, res, &error)) {
        g_warning ("Error: %s failed to disable: '%s'", modem_ctx->path, error->message);
        modem_schedule_retry_op_state (modem_object);
    } else {
        modem_schedule_op_state (modem_object, next_state);
    }

    g_object_unref (modem_object);
}

static gboolean
modem_op_state_run (MMObject *modem_object)
{
    ModemContext *modem_ctx = get_modem_context (modem_object);

    modem_ctx->next_id = 0;

    switch (modem_ctx->op_state) {
    case MODEM_OP_STATE_NONE:
        modem_schedule_op_state (modem_object, MODEM_OP_STATE_DISABLE);
        break;
    case MODEM_OP_STATE_DISABLE:
        g_message ("%s: disabling (try %d)...", modem_ctx->path, modem_ctx->tries);
        mm_modem_disable (modem_ctx->modem,
                          modem_ctx->cancellable,
                          (GAsyncReadyCallback) modem_disable_ready,
                          g_object_ref (modem_object));
        break;
    case MODEM_OP_STATE_LOW_POWER:
        /* Once disabled, move to low-power mode */
        g_message ("%s: setting low-power mode (try %d)...", modem_ctx->path, modem_ctx->tries);
        mm_modem_set_power_state (modem_ctx->modem,
                                  MM_MODEM_POWER_STATE_LOW,
                                  modem_ctx->cancellable,
                                  (GAsyncReadyCallback) set_power_state_low_ready,
                                  g_object_ref (modem_object));
        break;
    case MODEM_OP_STATE_ENABLE:
        /* Try to re-enable the modem */
        g_message ("%s: re-enabling (try %d)...", modem_ctx->path, modem_ctx->tries);
        mm_modem_enable (modem_ctx->modem,
                         modem_ctx->cancellable,
                         (GAsyncReadyCallback) modem_enable_ready,
                         g_object_ref (modem_object));

        break;
    case MODEM_OP_STATE_FINISH:
        g_message ("%s: modem kicked", modem_ctx->path);
        modem_context_cancel_op (modem_ctx);
        break;
    }

    return G_SOURCE_REMOVE;
}

static gboolean
reset_poll_cb (gpointer user_data)
{
    Context        *ctx = user_data;
    GHashTableIter  iter;
    gpointer        value;

    g_hash_table_iter_init (&iter, ctx->modems);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        MMObject     *modem_object = value;
        ModemContext *modem_ctx = get_modem_context (modem_object);
        gint64        now = g_get_monotonic_time ();
        gint64        time_failed;

#ifdef DEBUG
        modem_ctx->timestamp = now - 700 * G_USEC_PER_SEC;
#endif

        if (modem_ctx->timestamp == 0)
            continue;

        time_failed = now - modem_ctx->timestamp;
        if (time_failed > (KICK_INTERVAL_SECONDS * G_USEC_PER_SEC)) {
            g_message ("%s: idle/denied for %" G_GINT64_FORMAT " seconds; kicking...",
                       modem_ctx->path,
                       time_failed / G_USEC_PER_SEC);

            modem_context_cancel_op (modem_ctx);
            modem_ctx->cancellable = g_cancellable_new ();
            modem_op_state_run (modem_object);
        } else {
            g_message ("%s: not kicking yet; wait %" G_GINT64_FORMAT " seconds",
                       modem_ctx->path,
                       KICK_INTERVAL_SECONDS - (time_failed / G_USEC_PER_SEC));
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
term_handler (gpointer user_data)
{
    GMainLoop *main_loop = user_data;

    g_message ("Term received; quitting...");
    g_main_loop_quit (main_loop);
    return G_SOURCE_REMOVE;
}

int main (int argc, char **argv)
{
    Context *ctx;

    ctx = context_new ();

    g_unix_signal_add (SIGINT, term_handler, ctx->loop);
    g_unix_signal_add (SIGTERM, term_handler, ctx->loop);

    ctx->reset_poll_id = g_timeout_add_seconds (RESET_POLL_SECONDS, reset_poll_cb, ctx);

    g_bus_get (G_BUS_TYPE_SYSTEM, ctx->cancellable, bus_get_ready, ctx);
    g_main_loop_run (ctx->loop);

    context_free (ctx);
    return 0;
}
