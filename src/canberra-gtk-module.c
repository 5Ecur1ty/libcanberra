/***
  This file is part of libcanberra.

  Copyright 2008 Lennart Poettering

  libcanberra is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 2.1 of the
  License, or (at your option) any later version.

  libcanberra is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with libcanberra. If not, If not, see
  <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>

#include "canberra-gtk.h"

typedef struct {
    guint signal_id;
    GObject *object;
    GValue arg1;
    gboolean arg1_is_set;
    GdkEvent *event;
} SoundEventData;

/*
   We generate these sounds:

   dialog-error
   dialog-warning
   dialog-information
   dialog-question
   window-new
   window-close
   window-minimized
   window-unminimized
   window-maximized
   window-unmaximized
   notebook-tab-changed
   dialog-ok
   dialog-cancel
   item-selected
   link-pressed
   link-released
   button-pressed
   button-released
   menu-click
   button-toggle-on
   button-toggle-off
   menu-popup
   menu-popdown
   menu-replace
   tooltip-popup
   tooltip-popdown

   TODO:
   drag-start
   drag-accept
   drag-fail
   expander-toggle-on
   expander-toggle-off

*/

static GQueue sound_event_queue = G_QUEUE_INIT;

static int idle_id = 0;

static guint
    signal_id_dialog_response,
    signal_id_widget_show,
    signal_id_widget_hide,
    signal_id_check_menu_item_toggled,
    signal_id_menu_item_activate,
    signal_id_toggle_button_toggled,
    signal_id_button_pressed,
    signal_id_button_released,
    signal_id_widget_window_state_event,
    signal_id_notebook_switch_page,
    signal_id_tree_view_cursor_changed,
    signal_id_icon_view_selection_changed;

static GQuark
    disable_sound_quark,
    was_hidden_quark;

/* Make sure GCC doesn't warn us about a missing prototype for this
 * exported function */
void gtk_module_init(gint *argc, gchar ***argv[]);

static const char *translate_message_tye(GtkMessageType mt) {
    static const char *const message_type_table[] = {
        [GTK_MESSAGE_INFO] = "dialog-information",
        [GTK_MESSAGE_WARNING] = "dialog-warning",
        [GTK_MESSAGE_QUESTION] = "dialog-question",
        [GTK_MESSAGE_ERROR] = "dialog-error",
        [GTK_MESSAGE_OTHER] = NULL
    };

    if (mt >= G_N_ELEMENTS(message_type_table))
        return NULL;

    return message_type_table[mt];
}

static const char *translate_response(int response) {
    static const char *const response_table[] = {
        [-GTK_RESPONSE_NONE] = NULL,
        [-GTK_RESPONSE_REJECT] = "dialog-cancel",
        [-GTK_RESPONSE_DELETE_EVENT] = "dialog-cancel",
        [-GTK_RESPONSE_ACCEPT] = "dialog-ok",
        [-GTK_RESPONSE_OK] = "dialog-ok",
        [-GTK_RESPONSE_CANCEL] = "dialog-cancel",
        [-GTK_RESPONSE_CLOSE] = "dialog-ok",
        [-GTK_RESPONSE_YES] = "dialog-ok",
        [-GTK_RESPONSE_NO] = "dialog-cancel",
        [-GTK_RESPONSE_APPLY] = "dialog-ok",
        [-GTK_RESPONSE_HELP] = NULL,
    };

    if (response >= 0)
        return NULL;

    if ((unsigned) -response >= G_N_ELEMENTS(response_table))
        return NULL;

    return response_table[-response];
}

static gboolean is_child_of_combo_box(GtkWidget *w) {

    while (w) {

        if (GTK_IS_COMBO_BOX(w))
            return TRUE;

        w = gtk_widget_get_parent(w);
    }

    return FALSE;
}

static GtkDialog* find_parent_dialog(GtkWidget *w) {

    while (w) {

        if (GTK_IS_DIALOG(w))
            return GTK_DIALOG(w);

        w = gtk_widget_get_parent(w);
    }

    return NULL;
}

static void free_sound_event(SoundEventData *d) {

    g_object_unref(d->object);

    if (d->arg1_is_set)
        g_value_unset(&d->arg1);

    if (d->event)
        gdk_event_free(d->event);

    g_slice_free(SoundEventData, d);
}

static gboolean is_menu_hint(GdkWindowTypeHint hint) {
    return
        hint == GDK_WINDOW_TYPE_HINT_POPUP_MENU ||
        hint == GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU ||
        hint == GDK_WINDOW_TYPE_HINT_MENU;
}

static SoundEventData* filter_sound_event(SoundEventData *d) {
    GList *i, *n;

    do {

        for (i = sound_event_queue.head; i; i = n) {
            SoundEventData *j;

            j = i->data;
            n = i->next;

            if (d->object == j->object) {

                /* Let's drop a show event immediately followed by a
                 * hide event */

                if (d->signal_id == signal_id_widget_show &&
                    j->signal_id == signal_id_widget_hide) {

                    free_sound_event(d);
                    free_sound_event(j);
                    g_queue_delete_link(&sound_event_queue, i);

                    return NULL;
                }

                /* Let's drop widget hide events in favour of dialog
                 * response.
                 *
                 * Let's drop widget window state events in favour of
                 * widget hide/show.
                 *
                 * Let's drop double events */

                if ((d->signal_id == signal_id_widget_hide &&
                     j->signal_id == signal_id_dialog_response) ||

                    (d->signal_id == signal_id_widget_window_state_event &&
                     j->signal_id == signal_id_widget_hide) ||

                    (d->signal_id == signal_id_widget_window_state_event &&
                     j->signal_id == signal_id_widget_show)) {

                    free_sound_event(d);
                    d = j;
                    g_queue_delete_link(&sound_event_queue, i);
                    break;
                }

                if ((d->signal_id == signal_id_dialog_response &&
                     j->signal_id == signal_id_widget_hide) ||

                    (d->signal_id == signal_id_widget_show &&
                     j->signal_id == signal_id_widget_window_state_event) ||

                    (d->signal_id == signal_id_widget_hide &&
                     j->signal_id == signal_id_widget_window_state_event) ||

                    (d->signal_id == j->signal_id)) {

                    free_sound_event(j);
                    g_queue_delete_link(&sound_event_queue, i);
                }

            } else if (GTK_IS_WINDOW(d->object) && GTK_IS_WINDOW(j->object)) {

                GdkWindowTypeHint dhint, jhint;

                dhint = gtk_window_get_type_hint(GTK_WINDOW(d->object));
                jhint = gtk_window_get_type_hint(GTK_WINDOW(j->object));

                if (is_menu_hint(dhint) && is_menu_hint(jhint)) {

                    if (d->signal_id == signal_id_widget_hide &&
                        j->signal_id == signal_id_widget_show) {
                        free_sound_event(d);
                        d = j;
                        g_queue_delete_link(&sound_event_queue, i);
                        break;
                    }

                    if (d->signal_id == signal_id_widget_show &&
                        j->signal_id == signal_id_widget_hide) {

                        free_sound_event(j);
                        g_queue_delete_link(&sound_event_queue, i);
                    }
                }
            }
        }

        /* If we exited the iteration early, let's retry. */

    } while (i);

    /* FIXME: Filter menu hide on menu show */

    return d;
}

static gboolean is_hidden(GdkDisplay *d, GdkWindow *w) {
    Atom type_return;
    gint format_return;
    gulong nitems_return;
    gulong bytes_after_return;
    guchar *data = NULL;
    gboolean r = FALSE;

    if (XGetWindowProperty(GDK_DISPLAY_XDISPLAY(d), GDK_WINDOW_XID(w),
                            gdk_x11_get_xatom_by_name_for_display(d, "_NET_WM_STATE"),
                            0, G_MAXLONG, False, XA_ATOM, &type_return,
                            &format_return, &nitems_return, &bytes_after_return,
                            &data) != Success)
        return FALSE;

    if (type_return == XA_ATOM && format_return == 32 && data) {
        unsigned i;

        for (i = 0; i < nitems_return; i++) {
            Atom atom = ((Atom*) data)[i];

            if (atom == gdk_x11_get_xatom_by_name_for_display(d, "_NET_WM_STATE_HIDDEN")) {
                r = TRUE;
                break;
            }
        }
    }

    if (type_return != None && data != NULL)
        XFree (data);

    return r;
}

static void dispatch_sound_event(SoundEventData *d) {
    int ret = CA_SUCCESS;
    static gboolean menu_is_popped_up = TRUE;

    if (g_object_get_qdata(d->object, disable_sound_quark))
        return;

    if (d->signal_id == signal_id_widget_show) {
        GdkWindowTypeHint hint;

        /* Show/hide signals for non-windows have already been filtered out
         * by the emission hook! */

        hint = gtk_window_get_type_hint(GTK_WINDOW(d->object));

        if (is_menu_hint(hint)) {

            if (menu_is_popped_up) {
                ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                             CA_PROP_EVENT_ID, "menu-popup",
                                             CA_PROP_EVENT_DESCRIPTION, "Menu popped up",
                                             CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                             NULL);
            } else {
                ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                             CA_PROP_EVENT_ID, "menu-replace",
                                             CA_PROP_EVENT_DESCRIPTION, "Menu replaced",
                                             CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                             NULL);
            }

            menu_is_popped_up = TRUE;

        } else if (hint == GDK_WINDOW_TYPE_HINT_TOOLTIP) {

            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, "tooltip-popup",
                                         CA_PROP_EVENT_DESCRIPTION, "Tooltip popped up",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);

        } else if (hint == GDK_WINDOW_TYPE_HINT_NORMAL ||
                   hint == GDK_WINDOW_TYPE_HINT_DIALOG) {

            gboolean played_sound = FALSE;

            if (GTK_IS_MESSAGE_DIALOG(d->object)) {
                GtkMessageType mt;
                const char *id;

                g_object_get(d->object, "message_type", &mt, NULL);

                if ((id = translate_message_tye(mt))) {

                    ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                                 CA_PROP_EVENT_ID, id,
                                                 CA_PROP_EVENT_DESCRIPTION, "Message dialog shown",
                                                 CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                                 NULL);
                    played_sound = TRUE;
                }

            }

            if (!played_sound)
                ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                             CA_PROP_EVENT_ID, "window-new",
                                             CA_PROP_EVENT_DESCRIPTION, "Window shown",
                                             CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                             NULL);
        }
    }

    if (GTK_IS_DIALOG(d->object) && d->signal_id == signal_id_dialog_response) {

        int response;
        const char *id;

        response = g_value_get_int(&d->arg1);

        if ((id = translate_response(response))) {

            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, id,
                                         CA_PROP_EVENT_DESCRIPTION, "Dialog closed",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);
        } else {
            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, "window-close",
                                         CA_PROP_EVENT_DESCRIPTION, "Window closed",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);
        }

    } else if (d->signal_id == signal_id_widget_hide) {
        GdkWindowTypeHint hint;

        hint = gtk_window_get_type_hint(GTK_WINDOW(d->object));

        if (is_menu_hint(hint)) {

            if (GTK_IS_MENU(gtk_bin_get_child(GTK_BIN(d->object)))) {

                ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                             CA_PROP_EVENT_ID, "menu-popdown",
                                             CA_PROP_EVENT_DESCRIPTION, "Menu popped down",
                                             CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                             NULL);
            }

            menu_is_popped_up = FALSE;

        } else if (hint == GDK_WINDOW_TYPE_HINT_TOOLTIP) {

            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, "tooltip-popdown",
                                         CA_PROP_EVENT_DESCRIPTION, "Tooltip popped down",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);

        } else if (hint == GDK_WINDOW_TYPE_HINT_NORMAL ||
                   hint == GDK_WINDOW_TYPE_HINT_DIALOG) {

            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, "window-close",
                                         CA_PROP_EVENT_DESCRIPTION, "Window closed",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);
        }
    }

    if (GTK_IS_WINDOW(d->object) && d->signal_id == signal_id_widget_window_state_event) {
        GdkEventWindowState *e;
        gboolean h, ph;

        e = (GdkEventWindowState*) d->event;

        h = is_hidden(gdk_screen_get_display(gdk_event_get_screen(d->event)), e->window);
        ph = !!g_object_get_qdata(d->object, was_hidden_quark);
        g_object_set_qdata(d->object, was_hidden_quark, GINT_TO_POINTER(h));

        if ((e->changed_mask & GDK_WINDOW_STATE_ICONIFIED) && (e->new_window_state & GDK_WINDOW_STATE_ICONIFIED) && h && !ph) {

            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, "window-minimized",
                                         CA_PROP_EVENT_DESCRIPTION, "Window minimized",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);

        } else if ((e->changed_mask & (GDK_WINDOW_STATE_MAXIMIZED|GDK_WINDOW_STATE_FULLSCREEN)) && (e->new_window_state & (GDK_WINDOW_STATE_MAXIMIZED|GDK_WINDOW_STATE_FULLSCREEN))) {

            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, "window-maximized",
                                         CA_PROP_EVENT_DESCRIPTION, "Window maximized",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);

        } else if ((e->changed_mask & GDK_WINDOW_STATE_ICONIFIED) && !(e->new_window_state & GDK_WINDOW_STATE_ICONIFIED) && ph) {

            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, "window-unminimized",
                                         CA_PROP_EVENT_DESCRIPTION, "Window unminimized",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);
        } else if ((e->changed_mask & (GDK_WINDOW_STATE_MAXIMIZED|GDK_WINDOW_STATE_FULLSCREEN)) && !(e->new_window_state & (GDK_WINDOW_STATE_MAXIMIZED|GDK_WINDOW_STATE_FULLSCREEN))) {

            ret = ca_gtk_play_for_widget(GTK_WIDGET(d->object), 0,
                                         CA_PROP_EVENT_ID, "window-unmaximized",
                                         CA_PROP_EVENT_DESCRIPTION, "Window unmaximized",
                                         CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                         NULL);
        }
    }

    if (GTK_IS_CHECK_MENU_ITEM(d->object) && d->signal_id == signal_id_check_menu_item_toggled) {

        if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(d->object)))
            ret = ca_gtk_play_for_event(d->event, 0,
                                        CA_PROP_EVENT_ID, "button-toggle-on",
                                        CA_PROP_EVENT_DESCRIPTION, "Check menu item checked",
                                        CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                        NULL);
        else
            ret = ca_gtk_play_for_event(d->event, 0,
                                        CA_PROP_EVENT_ID, "button-toggle-off",
                                        CA_PROP_EVENT_DESCRIPTION, "Check menu item unchecked",
                                        CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                        NULL);

    } else if (GTK_IS_MENU_ITEM(d->object) && d->signal_id == signal_id_menu_item_activate) {

        if (!GTK_MENU_ITEM(d->object)->submenu)
            ret = ca_gtk_play_for_event(d->event, 0,
                                        CA_PROP_EVENT_ID, "menu-click",
                                        CA_PROP_EVENT_DESCRIPTION, "Menu item clicked",
                                        CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                        NULL);
    }

    if (GTK_IS_TOGGLE_BUTTON(d->object)) {

        if (d->signal_id == signal_id_toggle_button_toggled) {

            if (!is_child_of_combo_box(GTK_WIDGET(d->object))) {

                /* We don't want to play this sound if this is a toggle
                 * button belonging to combo box. */

                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->object)))
                    ret = ca_gtk_play_for_event(d->event, 0,
                                                CA_PROP_EVENT_ID, "button-toggle-on",
                                                CA_PROP_EVENT_DESCRIPTION, "Toggle button checked",
                                                CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                                NULL);
                else
                    ret = ca_gtk_play_for_event(d->event, 0,
                                                CA_PROP_EVENT_ID, "button-toggle-off",
                                                CA_PROP_EVENT_DESCRIPTION, "Toggle button unchecked",
                                                CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                                NULL);
            }
        }

    } else if (GTK_IS_LINK_BUTTON(d->object)) {

        if (d->signal_id == signal_id_button_pressed) {
            ret = ca_gtk_play_for_event(d->event, 0,
                                        CA_PROP_EVENT_ID, "link-pressed",
                                        CA_PROP_EVENT_DESCRIPTION, "Link pressed",
                                        CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                        NULL);

        } else if (d->signal_id == signal_id_button_released) {

            ret = ca_gtk_play_for_event(d->event, 0,
                                        CA_PROP_EVENT_ID, "link-released",
                                        CA_PROP_EVENT_DESCRIPTION, "Link released",
                                        CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                        NULL);
        }

    } else if (GTK_IS_BUTTON(d->object) && !GTK_IS_TOGGLE_BUTTON(d->object)) {

        if (d->signal_id == signal_id_button_pressed) {
            ret = ca_gtk_play_for_event(d->event, 0,
                                        CA_PROP_EVENT_ID, "button-pressed",
                                        CA_PROP_EVENT_DESCRIPTION, "Button pressed",
                                        CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                        NULL);

        } else if (d->signal_id == signal_id_button_released) {
            GtkDialog *dialog;
            gboolean dont_play = FALSE;

            if ((dialog = find_parent_dialog(GTK_WIDGET(d->object)))) {
                int response;

                /* Don't play the click sound if this is a response widget
                 * we will generate a dialog-xxx event sound anyway. */

                response = gtk_dialog_get_response_for_widget(dialog, GTK_WIDGET(d->object));
                dont_play = !!translate_response(response);
            }

            if (!dont_play)
                ret = ca_gtk_play_for_event(d->event, 0,
                                            CA_PROP_EVENT_ID, "button-released",
                                            CA_PROP_EVENT_DESCRIPTION, "Button released",
                                            CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                            NULL);
        }
    }

    if (GTK_IS_NOTEBOOK(d->object) && d->signal_id == signal_id_notebook_switch_page) {
        ret = ca_gtk_play_for_event(d->event, 0,
                                    CA_PROP_EVENT_ID, "notebook-tab-changed",
                                    CA_PROP_EVENT_DESCRIPTION, "Tab changed",
                                    CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                    NULL);
    }

    if (GTK_IS_TREE_VIEW(d->object) && d->signal_id == signal_id_tree_view_cursor_changed) {
        ret = ca_gtk_play_for_event(d->event, 0,
                                    CA_PROP_EVENT_ID, "item-selected",
                                    CA_PROP_EVENT_DESCRIPTION, "Item selected",
                                    CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                    NULL);
    }

    if (GTK_IS_ICON_VIEW(d->object) && d->signal_id == signal_id_icon_view_selection_changed) {
        ret = ca_gtk_play_for_event(d->event, 0,
                                    CA_PROP_EVENT_ID, "item-selected",
                                    CA_PROP_EVENT_DESCRIPTION, "Item selected",
                                    CA_PROP_CANBERRA_CACHE_CONTROL, "permanent",
                                    NULL);
    }

    if (ret != CA_SUCCESS)
        g_warning("Failed to play event sound: %s", ca_strerror(ret));
}

static gboolean idle_cb(void *userdata) {
    SoundEventData *d;

    idle_id = 0;

    while ((d = g_queue_pop_head(&sound_event_queue))) {

        if (!(d = filter_sound_event(d)))
            continue;

/*         g_message("Dispatching signal %s on %s", g_signal_name(d->signal_id), g_type_name(G_OBJECT_TYPE(d->object))); */

        dispatch_sound_event(d);
        free_sound_event(d);
    }

    return FALSE;
}

static gboolean emission_hook_cb(GSignalInvocationHint *hint, guint n_param_values, const GValue *param_values, gpointer data) {
    static SoundEventData *d = NULL;
    GdkEvent *e;
    GObject *object;

    object = g_value_get_object(&param_values[0]);

    /* Filter a few very often occuring signals as quickly as possible */
    if ((hint->signal_id == signal_id_widget_hide ||
         hint->signal_id == signal_id_widget_show ||
         hint->signal_id == signal_id_widget_window_state_event) &&
        !GTK_IS_WINDOW(object))
        return TRUE;

    if (hint->signal_id != signal_id_widget_hide &&
        hint->signal_id != signal_id_dialog_response &&
        !GTK_WIDGET_DRAWABLE(object))
        return TRUE;

/*     g_message("signal %s on %s", g_signal_name(hint->signal_id), g_type_name(G_OBJECT_TYPE(object))); */

    d = g_slice_new0(SoundEventData);

    d->object = g_object_ref(object);

    d->signal_id = hint->signal_id;

    if (d->signal_id == signal_id_widget_window_state_event) {
        d->event = gdk_event_copy(g_value_peek_pointer(&param_values[1]));
    } else if ((e = gtk_get_current_event()))
        d->event = gdk_event_copy(e);

    if (n_param_values > 1) {
        g_value_init(&d->arg1, G_VALUE_TYPE(&param_values[1]));
        g_value_copy(&param_values[1], &d->arg1);
        d->arg1_is_set = TRUE;
    }

    g_queue_push_tail(&sound_event_queue, d);

    if (idle_id == 0)
        idle_id = g_idle_add_full(GTK_PRIORITY_REDRAW-1, (GSourceFunc) idle_cb, NULL, NULL);

    return TRUE;
}

static void install_hook(GType type, const char *signal, guint *sn) {
    GTypeClass *type_class;

    type_class = g_type_class_ref(type);

    *sn = g_signal_lookup(signal, type);
    g_signal_add_emission_hook(*sn, 0, emission_hook_cb, NULL, NULL);

    g_type_class_unref(type_class);
}

void gtk_module_init(gint *argc, gchar ***argv[]) {
    /* This is the same quark libgnomeui uses! */
    disable_sound_quark = g_quark_from_string("gnome_disable_sound_events");
    was_hidden_quark = g_quark_from_string("canberra_was_hidden");

    install_hook(GTK_TYPE_WINDOW, "show", &signal_id_widget_show);
    install_hook(GTK_TYPE_WINDOW, "hide", &signal_id_widget_hide);
    install_hook(GTK_TYPE_DIALOG, "response", &signal_id_dialog_response);
    install_hook(GTK_TYPE_MENU_ITEM, "activate", &signal_id_menu_item_activate);
    install_hook(GTK_TYPE_CHECK_MENU_ITEM, "toggled", &signal_id_check_menu_item_toggled);
    install_hook(GTK_TYPE_TOGGLE_BUTTON, "toggled", &signal_id_toggle_button_toggled);
    install_hook(GTK_TYPE_BUTTON, "pressed", &signal_id_button_pressed);
    install_hook(GTK_TYPE_BUTTON, "released", &signal_id_button_released);
    install_hook(GTK_TYPE_WIDGET, "window-state-event", &signal_id_widget_window_state_event);
    install_hook(GTK_TYPE_NOTEBOOK, "switch-page", &signal_id_notebook_switch_page);
    install_hook(GTK_TYPE_TREE_VIEW, "cursor-changed", &signal_id_tree_view_cursor_changed);
    install_hook(GTK_TYPE_ICON_VIEW, "selection-changed", &signal_id_icon_view_selection_changed);
}