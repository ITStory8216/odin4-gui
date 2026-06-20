#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

// Globals
GSimpleAction *action_reboot;
GSimpleAction *action_nand_erase;
GSimpleAction *action_val_check;

GtkWidget *row_bl;
GtkWidget *row_ap;
GtkWidget *row_cp;
GtkWidget *row_csc;
GtkWidget *row_ums;

GtkStringList *dev_list;
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
GtkWidget *main_window = NULL;

// Helper to get text from AdwEntryRow
const char* get_row_text(GtkWidget *row) {
    return gtk_editable_get_text(GTK_EDITABLE(row));
}

void set_row_text(GtkWidget *row, const char *text) {
    gtk_editable_set_text(GTK_EDITABLE(row), text);
}

void update_dock_progress(double fraction, gboolean visible) {
    if (!dbus_conn) return;
    GVariantBuilder *b = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(b, "{sv}", "progress", g_variant_new_double(fraction));
    g_variant_builder_add(b, "{sv}", "progress-visible", g_variant_new_boolean(visible));
    g_dbus_connection_call(dbus_conn, "com.canonical.Unity", "/com/canonical/Unity/LauncherEntry",
        "com.canonical.Unity.LauncherEntry", "Update",
        g_variant_new("(sa{sv})", "application://odin4-gui.desktop", b),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

void send_notification(const gchar *summary, const gchar *body, const gchar *icon) {
    if (!dbus_conn) return;
    GVariantBuilder *hints = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_call(dbus_conn, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)", "Odin4", 0, icon, summary, body, NULL, hints, 5000),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

void append_log(const gchar *text) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_log));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, text, -1);
    
    GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(text_log), mark, 0.0, FALSE, 0.0, 0.0);
}

gboolean on_pulse_timer(gpointer data) {
    if (child_pid != 0) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress_bar));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "Connecting / Initializing...");
        return G_SOURCE_CONTINUE;
    }
    return G_SOURCE_REMOVE;
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
            int percentage = 0;
            if (p1 && sscanf(p1, "(%d%%)", &percentage) == 1) {
                is_progress_line = TRUE;
            } else {
                gchar *p2 = strstr(buf, "\"value\":");
                if (p2 && sscanf(p2, "\"value\":%d", &percentage) == 1) {
                    is_progress_line = TRUE;
                }
            }
            
            if (is_progress_line) {
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

            gchar *temp = g_strdup(buf);
            gchar *stripped = g_strstrip(temp);
            if (strlen(stripped) > 0 && !is_progress_line) {
                append_log(buf);
            }
            g_free(temp);
        }
    }
    
    if (condition & G_IO_HUP) {
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// Unused callback removed

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
        
        GtkWidget *dialog = adw_message_dialog_new(GTK_WINDOW(main_window), "Flashing Failed", error_text);
        adw_message_dialog_add_response(ADW_MESSAGE_DIALOG(dialog), "close", "Close");
        gtk_window_present(GTK_WINDOW(dialog));
        g_free(error_text);
        send_notification("Flashing Failed", "An error occurred. Check the log for details.", "dialog-error");
    }
    g_spawn_close_pid(pid);
    child_pid = 0;
    
    gtk_button_set_label(GTK_BUTTON(btn_start), "Start Flash");
    gtk_widget_remove_css_class(btn_start, "destructive-action");
    gtk_widget_add_css_class(btn_start, "suggested-action");
    
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
        // Clear list
        gtk_string_list_splice(dev_list, 0, g_list_model_get_n_items(G_LIST_MODEL(dev_list)), NULL);
        
        gchar **lines = g_strsplit(std_out_data, "\n", -1);
        int added = 0;
        for (int i = 0; lines[i] != NULL; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (strlen(line) > 0 && strstr(line, "odin4") == NULL && strstr(line, "Usage") == NULL) {
                gtk_string_list_append(dev_list, line);
                added++;
            }
        }
        g_strfreev(lines);
        g_free(std_out_data);
        
        if (added > 0) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(combo_device), 0);
        } else {
            gtk_string_list_append(dev_list, "No devices found");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(combo_device), 0);
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
    
    if (g_variant_get_boolean(g_action_get_state(G_ACTION(action_reboot))))
        g_ptr_array_add(args, g_strdup("--reboot"));
    if (g_variant_get_boolean(g_action_get_state(G_ACTION(action_nand_erase))))
        g_ptr_array_add(args, g_strdup("-e"));
    if (g_variant_get_boolean(g_action_get_state(G_ACTION(action_val_check))))
        g_ptr_array_add(args, g_strdup("-V"));
    
    const gchar *bl = get_row_text(row_bl);
    if (bl && strlen(bl) > 0) { g_ptr_array_add(args, g_strdup("-b")); g_ptr_array_add(args, g_strdup(bl)); }
    
    const gchar *ap = get_row_text(row_ap);
    if (ap && strlen(ap) > 0) { g_ptr_array_add(args, g_strdup("-a")); g_ptr_array_add(args, g_strdup(ap)); }
    
    const gchar *cp = get_row_text(row_cp);
    if (cp && strlen(cp) > 0) { g_ptr_array_add(args, g_strdup("-c")); g_ptr_array_add(args, g_strdup(cp)); }
    
    const gchar *csc = get_row_text(row_csc);
    if (csc && strlen(csc) > 0) { g_ptr_array_add(args, g_strdup("-s")); g_ptr_array_add(args, g_strdup(csc)); }
    
    const gchar *ums = get_row_text(row_ums);
    if (ums && strlen(ums) > 0) { g_ptr_array_add(args, g_strdup("-u")); g_ptr_array_add(args, g_strdup(ums)); }
    
    // Get selected device
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(combo_device));
    const char *dev = gtk_string_list_get_string(dev_list, selected);
    if (dev && g_strcmp0(dev, "No devices found") != 0 && strlen(dev) > 0) {
        g_ptr_array_add(args, g_strdup("-d"));
        g_ptr_array_add(args, g_strdup(dev));
    }
    
    g_ptr_array_add(args, NULL);
    gchar **argv = (gchar **)g_ptr_array_free(args, FALSE);
    
    append_log("Started flashing...\n");

    GError *error = NULL;
    gboolean success = g_spawn_async_with_pipes(
        NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
        NULL, NULL, &child_pid, NULL, &std_out, &std_err, &error);

    if (success) {
        gtk_button_set_label(GTK_BUTTON(btn_start), "Cancel Flash");
        gtk_widget_remove_css_class(btn_start, "suggested-action");
        gtk_widget_add_css_class(btn_start, "destructive-action");
        
        pulse_timer_id = g_timeout_add(100, on_pulse_timer, NULL);
        
        GIOChannel *out_ch = g_io_channel_unix_new(std_out);
        g_io_channel_set_flags(out_ch, G_IO_FLAG_NONBLOCK, NULL);
        g_io_add_watch(out_ch, G_IO_IN | G_IO_HUP, read_output, NULL);
        g_io_channel_unref(out_ch);

        GIOChannel *err_ch = g_io_channel_unix_new(std_err);
        g_io_channel_set_flags(err_ch, G_IO_FLAG_NONBLOCK, NULL);
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

static void on_file_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GtkWidget *row = GTK_WIDGET(user_data);
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, res, &error);
    
    if (file) {
        char *path = g_file_get_path(file);
        set_row_text(row, path);
        g_free(path);
        g_object_unref(file);
    } else {
        g_error_free(error);
    }
}

static void on_browse_clicked(GtkWidget *button, gpointer user_data) {
    GtkWidget *row = GTK_WIDGET(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();

    // Create file filters
    GtkFileFilter *firmware_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(firmware_filter, "Firmware Files (*.tar, *.md5, *.lz4, *.img, *.bin)");
    gtk_file_filter_add_pattern(firmware_filter, "*.tar");
    gtk_file_filter_add_pattern(firmware_filter, "*.md5");
    gtk_file_filter_add_pattern(firmware_filter, "*.tar.md5");
    gtk_file_filter_add_pattern(firmware_filter, "*.lz4");
    gtk_file_filter_add_pattern(firmware_filter, "*.img");
    gtk_file_filter_add_pattern(firmware_filter, "*.bin");

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All Files");
    gtk_file_filter_add_pattern(all_filter, "*");

    GListStore *store = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(store, firmware_filter);
    g_list_store_append(store, all_filter);

    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(store));
    gtk_file_dialog_set_default_filter(dialog, firmware_filter);
    g_object_unref(store);
    g_object_unref(firmware_filter);
    g_object_unref(all_filter);

    gtk_file_dialog_open(dialog, GTK_WINDOW(main_window), NULL, on_file_dialog_response, row);
    g_object_unref(dialog);
}

GtkWidget* create_file_row(const char *title, GtkWidget **row_out) {
    GtkWidget *row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    
    GtkWidget *btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(btn, "flat");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_browse_clicked), row);
    
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(row), btn);
    *row_out = row;
    return row;
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data) {
    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *list = g_value_get_boxed(value);
        GSList *files = gdk_file_list_get_files(list);
        for (GSList *l = files; l != NULL; l = l->next) {
            GFile *f = l->data;
            char *filename = g_file_get_path(f);
            if (filename) {
                gchar *basename = g_path_get_basename(filename);
                gchar *lower_base = g_ascii_strdown(basename, -1);
                
                gboolean valid_ext = g_str_has_suffix(lower_base, ".tar") ||
                                     g_str_has_suffix(lower_base, ".md5") ||
                                     g_str_has_suffix(lower_base, ".lz4") ||
                                     g_str_has_suffix(lower_base, ".img") ||
                                     g_str_has_suffix(lower_base, ".bin");

                if (valid_ext) {
                    if (g_str_has_prefix(basename, "AP_") || g_str_has_prefix(basename, "KIES_HOME_"))
                        set_row_text(row_ap, filename);
                    else if (g_str_has_prefix(basename, "BL_"))
                        set_row_text(row_bl, filename);
                    else if (g_str_has_prefix(basename, "CP_"))
                        set_row_text(row_cp, filename);
                    else if (g_str_has_prefix(basename, "CSC_") || g_str_has_prefix(basename, "HOME_CSC_"))
                        set_row_text(row_csc, filename);
                    else if (g_str_has_prefix(basename, "USERDATA_"))
                        set_row_text(row_ums, filename);
                } else {
                    append_log("Ignored dropped file (unsupported format): ");
                    append_log(basename);
                    append_log("\n");
                }
                
                g_free(lower_base);
                g_free(basename);
                g_free(filename);
            }
        }
        return TRUE;
    }
    return FALSE;
}

static void toggle_action(GSimpleAction *a, GVariant *p, gpointer d) {
    GVariant *s = g_action_get_state(G_ACTION(a));
    g_simple_action_set_state(a, g_variant_new_boolean(!g_variant_get_boolean(s)));
    g_variant_unref(s);
}

static void show_about(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    GtkApplication *app = GTK_APPLICATION(user_data);
    GtkWindow *win = gtk_application_get_active_window(app);
    GtkWidget *about = adw_about_window_new();
    adw_about_window_set_application_name(ADW_ABOUT_WINDOW(about), "Odin4 GUI");
    adw_about_window_set_version(ADW_ABOUT_WINDOW(about), "1.2");
    adw_about_window_set_developer_name(ADW_ABOUT_WINDOW(about), "Samsung Firmware Flasher GUI");
    gtk_window_set_transient_for(GTK_WINDOW(about), win);
    gtk_window_present(GTK_WINDOW(about));
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    main_window = adw_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(main_window), 750, 800);
    gtk_window_set_title(GTK_WINDOW(main_window), "Odin4 GUI");

    // Drag and drop setup for GTK4
    GtkDropTarget *target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(target, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(GTK_WIDGET(main_window), GTK_EVENT_CONTROLLER(target));

    // Menu Actions
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
    gtk_widget_insert_action_group(main_window, "opt", G_ACTION_GROUP(action_group));

    GMenu *menu = g_menu_new();
    GMenu *section = g_menu_new();
    g_menu_append(section, "Auto Reboot",      "opt.reboot");
    g_menu_append(section, "Nand Erase All",   "opt.nand-erase");
    g_menu_append(section, "Validation Check", "opt.val-check");
    g_menu_append_section(menu, "Flash Options", G_MENU_MODEL(section));

    GSimpleAction *about_action = g_simple_action_new("about", NULL);
    g_signal_connect(about_action, "activate", G_CALLBACK(show_about), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));

    GMenu *about_section = g_menu_new();
    g_menu_append(about_section, "About Odin4 GUI", "app.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(about_section));
    g_object_unref(section);

    // Main layout
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    
    // HeaderBar
    GtkWidget *header = adw_header_bar_new();
    
    // In Adwaita, to add a subtitle we use AdwWindowTitle
    GtkWidget *win_title = adw_window_title_new("Odin4 GUI", "Samsung Firmware Flasher");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), win_title);

    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    gtk_widget_add_css_class(menu_btn, "flat");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), menu_btn);

    btn_start = gtk_button_new_with_label("Start Flash");
    gtk_widget_add_css_class(btn_start, "suggested-action");
    g_signal_connect(btn_start, "clicked", G_CALLBACK(on_start_clicked), NULL);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_start);

    gtk_box_append(GTK_BOX(vbox), header);

    // Scrollable content wrapper for modern GNOME layout
    GtkWidget *main_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(main_scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), main_scroll);

    // Preferences Page to hold the groups natively
    GtkWidget *pref_page = adw_preferences_page_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(main_scroll), pref_page);

    // Files Group
    GtkWidget *group_files = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_files), "Firmware Files");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_files), create_file_row("BL", &row_bl));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_files), create_file_row("AP", &row_ap));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_files), create_file_row("CP", &row_cp));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_files), create_file_row("CSC", &row_csc));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_files), create_file_row("USERDATA", &row_ums));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(pref_page), ADW_PREFERENCES_GROUP(group_files));

    // Device Group
    GtkWidget *group_device = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_device), "Target Device");
    
    GtkWidget *dev_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dev_row), "Select Device");
    
    dev_list = gtk_string_list_new(NULL);
    combo_device = gtk_drop_down_new(G_LIST_MODEL(dev_list), NULL);
    gtk_widget_set_valign(combo_device, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(dev_row), combo_device);
    
    btn_refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_valign(btn_refresh, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(btn_refresh, "flat");
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    adw_action_row_add_suffix(ADW_ACTION_ROW(dev_row), btn_refresh);
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_device), dev_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(pref_page), ADW_PREFERENCES_GROUP(group_device));

    // Status Group
    GtkWidget *group_status = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_status), "Status &amp; Logs");

    // Progress Bar wrapped in a box for padding
    GtkWidget *prog_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(prog_box, 10);
    gtk_widget_set_margin_bottom(prog_box, 10);
    progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "Ready");
    gtk_box_append(GTK_BOX(prog_box), progress_bar);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_status), prog_box);

    // Terminal log natively styled
    GtkWidget *log_scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(log_scrolled), 250);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(log_scrolled), TRUE);
    
    text_log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_log), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_log), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_log), GTK_WRAP_WORD_CHAR);
    
    // Simple padding for a clean native text field
    gtk_widget_add_css_class(text_log, "log-view");
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "textview.log-view { padding: 12px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), 
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_scrolled), text_log);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_status), log_scrolled);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(pref_page), ADW_PREFERENCES_GROUP(group_status));

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(main_window), vbox);
    gtk_window_present(GTK_WINDOW(main_window));

    // Initial refresh
    on_refresh_clicked(NULL, NULL);
}

void self_register() {
    // reads the absolute path of the current executable file from /proc/self/exe
    char exe_path[4096] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) { g_printerr("self_register: failed to read /proc/self/exe\n"); return; }
    exe_path[len] = '\0';
    gchar *exe_dir = g_path_get_dirname(exe_path);
    const gchar *home = g_get_home_dir();
    g_printerr("self_register: exe=%s home=%s\n", exe_path, home);

    gchar *desktop_dir = g_build_filename(home, ".local", "share", "applications", NULL);
    gchar *new_desktop_path = g_build_filename(desktop_dir, "itstory.odin4.desktop", NULL);
    
    // clean up old shortcuts to prevent duplicate icons
    const gchar *old_names[] = {"odin4-gui.desktop", "com.example.odin4.desktop", NULL};
    for (int i = 0; old_names[i]; i++) {
        gchar *old_path = g_build_filename(desktop_dir, old_names[i], NULL);
        if (g_file_test(old_path, G_FILE_TEST_EXISTS)) remove(old_path);
        g_free(old_path);
    }

    g_mkdir_with_parents(desktop_dir, 0755);
    gchar *content = g_strdup_printf(
        "[Desktop Entry]\n"
        "Name=Odin4 GUI\n"
        "Comment=Samsung Firmware Flasher\n"
        "Exec=\"%s\"\n"
        "Path=%s\n"
        "Icon=emblem-downloads\n"
        "Terminal=false\n"
        "Type=Application\n"
        "Categories=Utility\n"
        "StartupWMClass=itstory.odin4\n"
        "StartupNotify=true\n",
        exe_path, exe_dir);

    // overwrite to ensure it's always up-to-date with current location
    GError *error = NULL;
    if (!g_file_set_contents(new_desktop_path, content, -1, &error)) {
        g_printerr("self_register: failed to write %s: %s\n", new_desktop_path, error->message);
        g_error_free(error);
    } else {
        g_printerr("self_register: wrote %s\n", new_desktop_path);
    }
    g_free(content);

    // force gnome shell to detect the change
    gchar *update_cmd = g_strdup_printf("update-desktop-database %s", desktop_dir);
    g_spawn_command_line_sync(update_cmd, NULL, NULL, NULL, NULL);
    g_free(update_cmd);
    utime(desktop_dir, NULL);  // touch directory to trigger inotify

    g_free(exe_dir);
    g_free(desktop_dir);
    g_free(new_desktop_path);
}

int main(int argc, char *argv[]) {
    self_register();
    dbus_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

    AdwApplication *app = adw_application_new("itstory.odin4", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    if (dbus_conn) g_object_unref(dbus_conn);
    return status;
}
