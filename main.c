#include <gtk/gtk.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

// options - gmenumodel / gsimpleaction based architecture
GSimpleAction *action_reboot;
GSimpleAction *action_nand_erase;
GSimpleAction *action_val_check;

GtkWidget *entry_bl;
GtkWidget *entry_ap;
GtkWidget *entry_cp;
GtkWidget *entry_csc;
GtkWidget *entry_ums;

GtkWidget *combo_device;
GtkWidget *btn_refresh;
GtkWidget *btn_start;
GtkWidget *text_log;
GtkWidget *progress_bar;

GPid child_pid = 0;
gint std_out = 0;
gint std_err = 0;
guint pulse_timer_id = 0;

GDBusConnection *dbus_conn = NULL;

void update_dock_progress(double fraction, gboolean visible) {
    if (!dbus_conn) return;

    GVariantBuilder *b = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(b, "{sv}", "progress", g_variant_new_double(fraction));
    g_variant_builder_add(b, "{sv}", "progress-visible", g_variant_new_boolean(visible));

    g_dbus_connection_call(
        dbus_conn,
        "com.canonical.Unity",
        "/com/canonical/Unity/LauncherEntry",
        "com.canonical.Unity.LauncherEntry",
        "Update",
        g_variant_new("(sa{sv})", "application://odin4-gui.desktop", b),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL, NULL, NULL
    );
}

void send_notification(const gchar *summary, const gchar *body, const gchar *icon) {
    if (!dbus_conn) return;

    GVariantBuilder *hints = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));

    g_dbus_connection_call(
        dbus_conn,
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify",
        g_variant_new("(susssasa{sv}i)",
            "Odin4",
            0,
            icon,
            summary,
            body,
            NULL,
            hints,
            5000
        ),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL, NULL, NULL
    );
}

void append_log(const gchar *text) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_log));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, text, -1);
    
    GtkTextMark *mark = gtk_text_buffer_get_insert(buffer);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(text_log), mark, 0.0, TRUE, 0.0, 1.0);
}

gboolean on_pulse_timer(gpointer data) {
    if (child_pid != 0) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress_bar));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "Connecting / Initializing...");
        return TRUE;
    }
    return FALSE;
}

gboolean read_output(GIOChannel *source, GIOCondition condition, gpointer data) {
    gchar buf[1024];
    gsize bytes_read;
    GError *error = NULL;

    if (condition & G_IO_IN) {
        GIOStatus status = g_io_channel_read_chars(source, buf, sizeof(buf) - 1, &bytes_read, &error);
        if (status == G_IO_STATUS_NORMAL && bytes_read > 0) {
            buf[bytes_read] = '\0';
            gboolean is_progress_line = FALSE;
            
            gchar *p1 = strstr(buf, "(");
            if (p1) {
                int percentage = 0;
                if (sscanf(p1, "(%d%%)", &percentage) == 1) {
                    is_progress_line = TRUE;
                    if (pulse_timer_id != 0) {
                        g_source_remove(pulse_timer_id);
                        pulse_timer_id = 0;
                    }
                    double frac = percentage / 100.0;
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), frac);
                    
                    gchar text[32];
                    g_snprintf(text, sizeof(text), "Flashing... %d%%", percentage);
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), text);
                    
                    update_dock_progress(frac, TRUE);
                }
            }
            
            gchar *p2 = strstr(buf, "\"value\":");
            if (p2) {
                int percentage = 0;
                if (sscanf(p2, "\"value\":%d", &percentage) == 1) {
                    is_progress_line = TRUE;
                    if (pulse_timer_id != 0) {
                        g_source_remove(pulse_timer_id);
                        pulse_timer_id = 0;
                    }
                    double frac = percentage / 100.0;
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), frac);
                    
                    gchar text[32];
                    g_snprintf(text, sizeof(text), "Flashing... %d%%", percentage);
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), text);
                    
                    update_dock_progress(frac, TRUE);
                }
            }

            gchar *temp = g_strdup(buf);
            gchar *stripped = g_strstrip(temp);
            if (strlen(stripped) > 0 && !is_progress_line) {
                append_log(buf);
            }
            g_free(temp);
        }
    }
    
    if (condition & G_IO_HUP) {
        return FALSE;
    }
    return TRUE;
}

void on_child_watch(GPid pid, gint status, gpointer user_data) {
    if (status == 0) {
        append_log("\nFlashing completed successfully!\n");
        send_notification("Flashing Complete", "Firmware has been flashed successfully.", "emblem-default");
    } else if (status == 9 || status == SIGKILL) {
        append_log("\nFlashing cancelled by user.\n");
        send_notification("Flashing Cancelled", "The flashing process was cancelled.", "dialog-warning");
    } else {
        append_log("\nFlashing stopped (Error).\n");
        
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_log));
        GtkTextIter start, end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        start = end;
        if (!gtk_text_iter_backward_chars(&start, 300)) {
            gtk_text_buffer_get_start_iter(buffer, &start);
        }
        gchar *error_text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        
        GtkWidget *toplevel = gtk_widget_get_toplevel(btn_start);
        if (gtk_widget_is_toplevel(toplevel)) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(toplevel),
                                                       GTK_DIALOG_DESTROY_WITH_PARENT,
                                                       GTK_MESSAGE_ERROR,
                                                       GTK_BUTTONS_CLOSE,
                                                       "Flashing Failed");
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                                     "Last log output:\n\n%s", error_text);
            g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
            gtk_widget_show_all(dialog);
        }
        g_free(error_text);
        send_notification("Flashing Failed", "An error occurred. Check the log for details.", "dialog-error");
    }
    g_spawn_close_pid(pid);
    child_pid = 0;
    
    // revert button back to start
    gtk_button_set_label(GTK_BUTTON(btn_start), "Start Flash");
    GtkStyleContext *context = gtk_widget_get_style_context(btn_start);
    gtk_style_context_remove_class(context, "destructive-action");
    gtk_style_context_add_class(context, "suggested-action");
    
    // reset progress bar
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "Ready");
    
    if (pulse_timer_id != 0) {
        g_source_remove(pulse_timer_id);
        pulse_timer_id = 0;
    }
    
    update_dock_progress(0.0, FALSE);
}

void on_refresh_clicked(GtkWidget *widget, gpointer data) {
    gchar *argv[] = {"./odin4", "-l", NULL};
    gchar *std_out_data = NULL;
    GError *error = NULL;
    
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &std_out_data, NULL, NULL, &error)) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo_device));
        
        gchar **lines = g_strsplit(std_out_data, "\n", -1);
        int added = 0;
        for (int i = 0; lines[i] != NULL; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (strlen(line) > 0 && strstr(line, "odin4") == NULL && strstr(line, "Usage") == NULL) {
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_device), line);
                added++;
            }
        }
        g_strfreev(lines);
        g_free(std_out_data);
        
        if (added > 0) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo_device), 0);
        } else {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_device), "No devices found");
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo_device), 0);
        }
        
        append_log("Device list refreshed.\n");
    } else {
        append_log("Failed to refresh device list: ");
        append_log(error->message);
        append_log("\n");
        g_error_free(error);
    }
}

void on_start_clicked(GtkWidget *widget, gpointer data) {
    if (child_pid != 0) {
        append_log("\nCancelling flash...\n");
        kill(child_pid, SIGKILL);
        return;
    }

    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, g_strdup("./odin4"));
    
    if (g_variant_get_boolean(g_action_get_state(G_ACTION(action_reboot)))) {
        g_ptr_array_add(args, g_strdup("--reboot"));
    }
    if (g_variant_get_boolean(g_action_get_state(G_ACTION(action_nand_erase)))) {
        g_ptr_array_add(args, g_strdup("-e"));
    }
    if (g_variant_get_boolean(g_action_get_state(G_ACTION(action_val_check)))) {
        g_ptr_array_add(args, g_strdup("-V"));
    }
    
    const gchar *bl = gtk_entry_get_text(GTK_ENTRY(entry_bl));
    if (bl && strlen(bl) > 0) { g_ptr_array_add(args, g_strdup("-b")); g_ptr_array_add(args, g_strdup(bl)); }
    
    const gchar *ap = gtk_entry_get_text(GTK_ENTRY(entry_ap));
    if (ap && strlen(ap) > 0) { g_ptr_array_add(args, g_strdup("-a")); g_ptr_array_add(args, g_strdup(ap)); }
    
    const gchar *cp = gtk_entry_get_text(GTK_ENTRY(entry_cp));
    if (cp && strlen(cp) > 0) { g_ptr_array_add(args, g_strdup("-c")); g_ptr_array_add(args, g_strdup(cp)); }
    
    const gchar *csc = gtk_entry_get_text(GTK_ENTRY(entry_csc));
    if (csc && strlen(csc) > 0) { g_ptr_array_add(args, g_strdup("-s")); g_ptr_array_add(args, g_strdup(csc)); }
    
    const gchar *ums = gtk_entry_get_text(GTK_ENTRY(entry_ums));
    if (ums && strlen(ums) > 0) { g_ptr_array_add(args, g_strdup("-u")); g_ptr_array_add(args, g_strdup(ums)); }
    
    gchar *dev = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_device));
    if (dev && g_strcmp0(dev, "No devices found") != 0 && strlen(dev) > 0) {
        g_ptr_array_add(args, g_strdup("-d"));
        g_ptr_array_add(args, dev);
    } else if (dev) {
        g_free(dev);
    }
    
    g_ptr_array_add(args, NULL);
    
    gchar **argv = (gchar **)g_ptr_array_free(args, FALSE);
    
    append_log("Started flashing...\n");

    GError *error = NULL;
    gboolean success = g_spawn_async_with_pipes(
        NULL, argv, NULL,
        G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
        NULL, NULL,
        &child_pid, NULL, &std_out, &std_err, &error
    );

    if (success) {
        gtk_button_set_label(GTK_BUTTON(btn_start), "Cancel Flash");
        GtkStyleContext *context = gtk_widget_get_style_context(btn_start);
        gtk_style_context_remove_class(context, "suggested-action");
        gtk_style_context_add_class(context, "destructive-action");
        
        pulse_timer_id = g_timeout_add(100, on_pulse_timer, NULL);
        
        GIOChannel *out_ch = g_io_channel_unix_new(std_out);
        g_io_channel_set_flags(out_ch, G_IO_FLAG_NONBLOCK, NULL);
        g_io_channel_set_encoding(out_ch, NULL, NULL); 
        g_io_add_watch(out_ch, G_IO_IN | G_IO_HUP, read_output, NULL);
        g_io_channel_unref(out_ch);

        GIOChannel *err_ch = g_io_channel_unix_new(std_err);
        g_io_channel_set_flags(err_ch, G_IO_FLAG_NONBLOCK, NULL);
        g_io_channel_set_encoding(err_ch, NULL, NULL);
        g_io_add_watch(err_ch, G_IO_IN | G_IO_HUP, read_output, NULL);
        g_io_channel_unref(err_ch);

        g_child_watch_add(child_pid, on_child_watch, NULL);
    } else {
        append_log("Error starting process: ");
        append_log(error->message);
        append_log("\n");
        g_error_free(error);
    }
    g_strfreev(argv);
}

typedef struct {
    GtkWidget *entry;
    GtkWindow *parent_window;
} FileRowData;

static void on_browse_clicked(GtkWidget *button, gpointer user_data) {
    FileRowData *data = (FileRowData *)user_data;
    GtkFileChooserNative *native = gtk_file_chooser_native_new(
        "Select File",
        data->parent_window,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Open",
        "_Cancel"
    );
    
    gint res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(native));
    if (res == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(native));
        gtk_entry_set_text(GTK_ENTRY(data->entry), filename);
        g_free(filename);
    }
    g_object_unref(native);
}

GtkWidget* create_section_label(const char *text) {
    GtkWidget *lbl = gtk_label_new(NULL);

    // gnome settings style: uppercase + small + bold + dimmed (dim-label)
    gchar *upper = g_utf8_strup(text, -1);
    gchar *markup = g_strdup_printf(
        "<span size='small' weight='bold'>%s</span>", upper);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    g_free(upper);
    g_free(markup);

    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_top(lbl, 18);
    gtk_widget_set_margin_bottom(lbl, 4);
    gtk_widget_set_margin_start(lbl, 2);

    // 'dim-label' css class: gtk theme renders this as muted/grey automatically
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "dim-label");

    return lbl;
}

GtkWidget* create_switch_row(const char *label_text, GtkWidget **switch_out) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(lbl, TRUE);
    
    *switch_out = gtk_switch_new();
    gtk_widget_set_valign(*switch_out, GTK_ALIGN_CENTER);
    
    gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), *switch_out, FALSE, FALSE, 0);
    return box;
}

GtkWidget* create_file_row(const char *label_text, GtkWidget **entry_out, GtkWidget *parent_window) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_size_request(lbl, 100, -1);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    
    *entry_out = gtk_entry_new();
    gtk_widget_set_hexpand(*entry_out, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(*entry_out), "No file selected...");
    
    GtkWidget *btn = gtk_button_new_with_label("Browse");
    
    FileRowData *data = g_new(FileRowData, 1);
    data->entry = *entry_out;
    data->parent_window = GTK_WINDOW(parent_window);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_browse_clicked), data);
    
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), *entry_out, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    return box;
}

enum {
    TARGET_URI_LIST
};

static GtkTargetEntry drag_targets[] = {
    { (gchar *)"text/uri-list", 0, TARGET_URI_LIST }
};

void on_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                           GtkSelectionData *data, guint info, guint time, gpointer user_data) {
    if (info == TARGET_URI_LIST) {
        gchar **uris = gtk_selection_data_get_uris(data);
        if (uris) {
            for (int i = 0; uris[i] != NULL; i++) {
                gchar *filename = g_filename_from_uri(uris[i], NULL, NULL);
                if (filename) {
                    gchar *basename = g_path_get_basename(filename);
                    if (g_str_has_prefix(basename, "AP_") || g_str_has_prefix(basename, "KIES_HOME_")) {
                        gtk_entry_set_text(GTK_ENTRY(entry_ap), filename);
                    } else if (g_str_has_prefix(basename, "BL_")) {
                        gtk_entry_set_text(GTK_ENTRY(entry_bl), filename);
                    } else if (g_str_has_prefix(basename, "CP_")) {
                        gtk_entry_set_text(GTK_ENTRY(entry_cp), filename);
                    } else if (g_str_has_prefix(basename, "CSC_") || g_str_has_prefix(basename, "HOME_CSC_")) {
                        gtk_entry_set_text(GTK_ENTRY(entry_csc), filename);
                    } else if (g_str_has_prefix(basename, "USERDATA_")) {
                        gtk_entry_set_text(GTK_ENTRY(entry_ums), filename);
                    }
                    g_free(basename);
                    g_free(filename);
                }
            }
            g_strfreev(uris);
        }
        gtk_drag_finish(context, TRUE, FALSE, time);
    } else {
        gtk_drag_finish(context, FALSE, FALSE, time);
    }
}

static void toggle_action(GSimpleAction *a, GVariant *p, gpointer d) {
    GVariant *s = g_action_get_state(G_ACTION(a));
    g_simple_action_set_state(a, g_variant_new_boolean(!g_variant_get_boolean(s)));
    g_variant_unref(s);
}

void self_register() {
    // reads the absolute path of the current executable file from /proc/self/exe
    char exe_path[4096] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) return;
    exe_path[len] = '\0';

    // directory contains executable file
    gchar *exe_dir = g_path_get_dirname(exe_path);

    // stores .desktop file to ~/.local/share/applications/odin4-gui.desktop
    const gchar *home = g_get_home_dir();
    gchar *desktop_dir = g_build_filename(home, ".local", "share", "applications", NULL);
    gchar *desktop_path = g_build_filename(desktop_dir, "odin4-gui.desktop", NULL);

    // do nothing if file exsists
    if (!g_file_test(desktop_path, G_FILE_TEST_EXISTS)) {
        g_mkdir_with_parents(desktop_dir, 0755);

        gchar *content = g_strdup_printf(
            "[Desktop Entry]\n"
            "Name=Odin4\n"
            "Comment=Samsung Firmware Flasher\n"
            "Exec=%s\n"
            "Path=%s\n"
            "Icon=system-software-install\n"
            "Terminal=false\n"
            "Type=Application\n"
            "Categories=Utility;System;\n"
            "StartupWMClass=odin4_gui\n"
            "StartupNotify=true\n",
            exe_path, exe_dir
        );

        g_file_set_contents(desktop_path, content, -1, NULL);
        g_free(content);

        g_spawn_command_line_async("update-desktop-database ~/.local/share/applications", NULL);
    }

    g_free(exe_dir);
    g_free(desktop_dir);
    g_free(desktop_path);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    // register app to overview on first launch
    self_register();

    dbus_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 750);
    gtk_window_set_icon_name(GTK_WINDOW(window), "system-software-install");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    
    // enable drag and drop from file manager
    gtk_drag_dest_set(window, GTK_DEST_DEFAULT_ALL, drag_targets, G_N_ELEMENTS(drag_targets), GDK_ACTION_COPY);
    g_signal_connect(window, "drag-data-received", G_CALLBACK(on_drag_data_received), NULL);

    // native header bar
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Odin4 GUI");
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    // --- hamburger menu (gmenumodel + gsimpleaction) ---

    // 1. create action group and register stateful toggle actions
    GSimpleActionGroup *action_group = g_simple_action_group_new();

    action_reboot     = g_simple_action_new_stateful("reboot",     NULL, g_variant_new_boolean(TRUE));
    action_nand_erase = g_simple_action_new_stateful("nand-erase", NULL, g_variant_new_boolean(FALSE));
    action_val_check  = g_simple_action_new_stateful("val-check",  NULL, g_variant_new_boolean(FALSE));

    g_signal_connect(action_reboot,     "activate", G_CALLBACK(toggle_action), NULL);
    g_signal_connect(action_nand_erase, "activate", G_CALLBACK(toggle_action), NULL);
    g_signal_connect(action_val_check,  "activate", G_CALLBACK(toggle_action), NULL);

    g_action_map_add_action(G_ACTION_MAP(action_group), G_ACTION(action_reboot));
    g_action_map_add_action(G_ACTION_MAP(action_group), G_ACTION(action_nand_erase));
    g_action_map_add_action(G_ACTION_MAP(action_group), G_ACTION(action_val_check));

    // 2. attach action group to window (prefix: "opt")
    gtk_widget_insert_action_group(window, "opt", G_ACTION_GROUP(action_group));

    // 3. define menu structure with gmenumodel
    // passing a label as the 2nd arg to g_menu_append_section renders it as a section header
    GMenu *menu = g_menu_new();

    GMenu *section = g_menu_new();
    g_menu_append(section, "Auto Reboot",      "opt.reboot");
    g_menu_append(section, "Nand Erase All",   "opt.nand-erase");
    g_menu_append(section, "Validation Check", "opt.val-check");
    g_menu_append_section(menu, "Flash Options", G_MENU_MODEL(section));
    g_object_unref(section);

    // 4. attach menu model to menu button
    GtkWidget *menu_btn = gtk_menu_button_new();
    GtkWidget *menu_icon = gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(menu_btn), menu_icon);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    g_object_unref(menu);

    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), menu_btn);
    // ---

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 20); // inner margin
    gtk_container_add(GTK_CONTAINER(window), main_vbox);

    // files section
    gtk_box_pack_start(GTK_BOX(main_vbox), create_section_label("Files"), FALSE, FALSE, 0);

    GtkWidget *vbox_files = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox_files, 10);
    
    gtk_box_pack_start(GTK_BOX(vbox_files), create_file_row("BL", &entry_bl, window), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_files), create_file_row("AP", &entry_ap, window), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_files), create_file_row("CP", &entry_cp, window), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_files), create_file_row("CSC", &entry_csc, window), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_files), create_file_row("USERDATA", &entry_ums, window), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_vbox), vbox_files, FALSE, FALSE, 0);

    // separator
    gtk_box_pack_start(GTK_BOX(main_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 5);

    // device section
    gtk_box_pack_start(GTK_BOX(main_vbox), create_section_label("Device"), FALSE, FALSE, 0);

    GtkWidget *hbox_device = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(hbox_device, 10);
    
    combo_device = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_device), "No devices found");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_device), 0);
    
    btn_refresh = gtk_button_new_with_label("Refresh List");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    
    gtk_box_pack_start(GTK_BOX(hbox_device), combo_device, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_device), btn_refresh, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(main_vbox), hbox_device, FALSE, FALSE, 0);

    // separator
    gtk_box_pack_start(GTK_BOX(main_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 10);

    // progress Bar
    progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "Ready");
    gtk_box_pack_start(GTK_BOX(main_vbox), progress_bar, FALSE, FALSE, 5);

    // log section - minimum height for readability
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(scrolled, -1, 200);
    text_log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_log), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_log), TRUE);
    gtk_container_add(GTK_CONTAINER(scrolled), text_log);
    
    // start flash button in header bar
    btn_start = gtk_button_new_with_label("Start Flash");
    GtkStyleContext *context = gtk_widget_get_style_context(btn_start);
    gtk_style_context_add_class(context, "suggested-action");
    g_signal_connect(btn_start, "clicked", G_CALLBACK(on_start_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), btn_start);

    gtk_box_pack_start(GTK_BOX(main_vbox), scrolled, TRUE, TRUE, 0);

    gtk_widget_show_all(window);
    
    // initial device list refresh
    on_refresh_clicked(NULL, NULL);

    gtk_main();

    if (dbus_conn) g_object_unref(dbus_conn);

    return 0;
}
