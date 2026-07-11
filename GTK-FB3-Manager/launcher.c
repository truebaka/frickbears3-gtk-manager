// ============================================================
// Frickbears 3 Manager — GTK4 версия (на чистом C)
// Функционал идентичен bash+whiptail варианту:
//   - импорт версий игры / модов (замена) / custom guards (добавление)
//   - сборка билдов версия+мод, переключение активного билда (симлинк)
//   - список гвардов с переключателем вкл/выкл (перемещение папки)
//   - удаление гвардов, компиляция C-лаунчера + .desktop файл
//   - очистка старых версий/модов/билдов
// ============================================================

#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include "lang.h"
#include <sys/stat.h>
#include <ctype.h>

// ---------- Пути (заполняются в main()) ----------
static char g_home[PATH_MAX];
static char g_base[PATH_MAX];
static char g_versions_dir[PATH_MAX];
static char g_mods_dir[PATH_MAX];
static char g_builds_dir[PATH_MAX];
static char g_current_link[PATH_MAX];
static char g_disabled_guards_dir[PATH_MAX];
static char g_bin_dir[PATH_MAX];
static char g_launcher_path[PATH_MAX];
static const char *GAME_EXE = "Frickbears3.exe";

// ---------- Виджеты главного окна ----------
typedef struct {
    GtkWidget *window;
    GtkWidget *label_build;
    GtkWidget *listbox_guards;
} AppCtx;

static AppCtx g_app;

// ============================================================
// Утилиты
// ============================================================

static void ensure_dir(const char *path) {
    g_mkdir_with_parents(path, 0755);
}

// Выполнить shell-команду, аргументы уже должны быть безопасно
// экранированы через g_shell_quote() перед вызовом.
static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    return rc;
}

static void show_info(const char *text) {
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", text);
    gtk_alert_dialog_show(dlg, GTK_WINDOW(g_app.window));
    g_object_unref(dlg);
}

// Локализованный текст кнопок подтверждения
static const char *btn_cancel_text(void) { return tr("Отмена", "Cancel"); }
static const char *btn_yes_text(void)    { return tr("Да", "Yes"); }

// Простое диалоговое подтверждение (да/нет), синхронный колбэк
typedef void (*ConfirmCb)(gboolean confirmed, gpointer user_data);

typedef struct {
    ConfirmCb cb;
    gpointer user_data;
} ConfirmCtx;

static void on_confirm_finish(GObject *src, GAsyncResult *res, gpointer user_data) {
    ConfirmCtx *ctx = user_data;
    GError *err = NULL;
    int idx = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
    if (err) g_error_free(err);
    // buttons: [0]="Отмена" [1]="Да"
    ctx->cb(idx == 1, ctx->user_data);
    g_free(ctx);
}

static void ask_confirm(const char *text, ConfirmCb cb, gpointer user_data) {
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", text);
    const char *buttons[] = {btn_cancel_text(), btn_yes_text(), NULL};
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 1);
    ConfirmCtx *ctx = g_new0(ConfirmCtx, 1);
    ctx->cb = cb;
    ctx->user_data = user_data;
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(g_app.window), NULL, on_confirm_finish, ctx);
    g_object_unref(dlg);
}

// Список подпапок директории (только имена), возвращает GList<char*> (нужно g_list_free_full(list, g_free))
static GList *list_subdirs(const char *dir) {
    GList *result = NULL;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            result = g_list_insert_sorted(result, g_strdup(entry->d_name), (GCompareFunc)g_strcmp0);
        }
    }
    closedir(d);
    return result;
}

// Найти папку addons/ кастомных гвардов внутри Wine-префикса
// Возвращает TRUE если найдена, путь пишется в out (размер outsz)
static gboolean find_guards_addon_dir(char *out, size_t outsz) {
    const char *prefix = g_getenv("WINEPREFIX");
    char appdata[PATH_MAX];
    if (prefix && *prefix) {
        snprintf(appdata, sizeof(appdata), "%s/drive_c/users/%s/AppData/Local", prefix, g_get_user_name());
    } else {
        snprintf(appdata, sizeof(appdata), "%s/.wine/drive_c/users/%s/AppData/Local", g_home, g_get_user_name());
    }

    DIR *d = opendir(appdata);
    if (!d) return FALSE;
    struct dirent *entry;
    gboolean found = FALSE;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        // case-insensitive поиск "frickbear" в имени
        char lower[512];
        g_strlcpy(lower, entry->d_name, sizeof(lower));
        for (char *p = lower; *p; p++) *p = tolower((unsigned char)*p);
        if (strstr(lower, "frickbear")) {
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", appdata, entry->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(out, outsz, "%s/addons", full);
                found = TRUE;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

// ============================================================
// Обновление списка гвардов (сканирует addons/ и guards_disabled/)
// ============================================================

typedef struct {
    char name[256];
    char full_path[PATH_MAX];
    gboolean enabled;
} GuardRowData;

static void free_guard_row_data(gpointer data) {
    g_free(data);
}

static void on_delete_guard_confirmed(gboolean confirmed, gpointer user_data);
static void on_guard_switch_state_set_deferred(gpointer user_data);

static gboolean idle_refresh_guards(gpointer data) {
    (void)data;
    extern void refresh_guards_list(void);
    refresh_guards_list();
    return G_SOURCE_REMOVE;
}

typedef struct {
    GuardRowData *row;
    gboolean want_enabled;
} SwitchToggleCtx;

static void perform_guard_toggle(SwitchToggleCtx *ctx) {
    char addons_dir[PATH_MAX];
    if (!find_guards_addon_dir(addons_dir, sizeof(addons_dir))) {
        show_info(tr("Папка addons/ не найдена. Запусти игру хотя бы раз.",
                      "The addons/ folder was not found. Launch the game at least once."));
        g_free(ctx);
        return;
    }
    ensure_dir(addons_dir);
    ensure_dir(g_disabled_guards_dir);

    char *src_q = g_shell_quote(ctx->row->full_path);
    char dest_path[PATH_MAX];
    if (ctx->want_enabled) {
        snprintf(dest_path, sizeof(dest_path), "%s/%s", addons_dir, ctx->row->name);
    } else {
        snprintf(dest_path, sizeof(dest_path), "%s/%s", g_disabled_guards_dir, ctx->row->name);
    }
    char *dest_q = g_shell_quote(dest_path);

    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "mv %s %s", src_q, dest_q);
    run_cmd(cmd);

    g_free(src_q);
    g_free(dest_q);
    g_free(ctx);

    g_idle_add(idle_refresh_guards, NULL);
}

static void on_toggle_confirmed(gboolean confirmed, gpointer user_data) {
    SwitchToggleCtx *ctx = user_data;
    if (!confirmed) {
        g_free(ctx);
        g_idle_add(idle_refresh_guards, NULL); // вернуть переключатель в прежнее состояние
        return;
    }
    perform_guard_toggle(ctx);
}

static void on_delete_confirmed(gboolean confirmed, gpointer user_data) {
    GuardRowData *row = user_data;
    if (confirmed) {
        char *path_q = g_shell_quote(row->full_path);
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", path_q);
        run_cmd(cmd);
        g_free(path_q);
        g_idle_add(idle_refresh_guards, NULL);
    }
    g_free(row);
}

static void on_delete_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuardRowData *src = user_data;
    GuardRowData *copy = g_new0(GuardRowData, 1);
    *copy = *src;
    char text[600];
    snprintf(text, sizeof(text),
             tr("Удалить гварда «%s» НАВСЕГДА?\nЭто действие нельзя отменить.",
                "Delete guard \"%s\" PERMANENTLY?\nThis action cannot be undone."),
             src->name);
    ask_confirm(text, on_delete_confirmed, copy);
}

static void on_switch_state_set(GtkSwitch *sw, gboolean state, gpointer user_data) {
    (void)sw;
    GuardRowData *src = user_data;
    if (state == src->enabled) return; // ничего не изменилось

    GuardRowData *copy = g_new0(GuardRowData, 1);
    *copy = *src;
    SwitchToggleCtx *ctx = g_new0(SwitchToggleCtx, 1);
    ctx->row = copy;
    ctx->want_enabled = state;

    char text[600];
    snprintf(text, sizeof(text),
             tr("%s гварда «%s»?", "%s guard \"%s\"?"),
             state ? tr("Включить", "Enable") : tr("Выключить", "Disable"),
             src->name);
    ask_confirm(text, on_toggle_confirmed, ctx);

    // сразу планируем refresh, чтобы визуальное состояние переключателя
    // синхронизировалось с реальным местоположением папки после решения
}

// g_signal_connect для "state-set" требует gboolean возврат;
// оборачиваем, чтобы GTK не применял состояние сам — обновим сами через refresh
static gboolean on_switch_state_set_wrapper(GtkSwitch *sw, gboolean state, gpointer user_data) {
    on_switch_state_set(sw, state, user_data);
    return TRUE; // блокируем немедленное изменение, дождёмся подтверждения + refresh
}

static GtkWidget *build_guard_row(const char *name, const char *full_path, gboolean enabled) {
    GuardRowData *data = g_new0(GuardRowData, 1);
    g_strlcpy(data->name, name, sizeof(data->name));
    g_strlcpy(data->full_path, full_path, sizeof(data->full_path));
    data->enabled = enabled;

    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(row_box, 8);
    gtk_widget_set_margin_end(row_box, 8);
    gtk_widget_set_margin_top(row_box, 6);
    gtk_widget_set_margin_bottom(row_box, 6);

    GtkWidget *label = gtk_label_new(name);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(row_box), label);

    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), enabled);
    gtk_switch_set_state(GTK_SWITCH(sw), enabled);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    // Данные привязываем к времени жизни виджета
    g_object_set_data_full(G_OBJECT(sw), "guard-data", data, free_guard_row_data);
    g_signal_connect(sw, "state-set", G_CALLBACK(on_switch_state_set_wrapper), data);
    gtk_box_append(GTK_BOX(row_box), sw);

    GtkWidget *del_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_set_tooltip_text(del_btn, tr("Удалить гварда навсегда", "Delete guard permanently"));
    // отдельная копия данных для кнопки удаления (свой список данных, не связан с switch)
    GuardRowData *del_data = g_new0(GuardRowData, 1);
    *del_data = *data;
    g_object_set_data_full(G_OBJECT(del_btn), "guard-data", del_data, free_guard_row_data);
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_button_clicked), del_data);
    gtk_box_append(GTK_BOX(row_box), del_btn);

    return row_box;
}

void refresh_guards_list(void) {
    // Очищаем текущий список
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(g_app.listbox_guards)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(g_app.listbox_guards), child);
    }

    char addons_dir[PATH_MAX];
    gboolean have_addons = find_guards_addon_dir(addons_dir, sizeof(addons_dir));

    ensure_dir(g_disabled_guards_dir);

    if (have_addons) {
        ensure_dir(addons_dir);
        GList *enabled_list = list_subdirs(addons_dir);
        for (GList *l = enabled_list; l; l = l->next) {
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", addons_dir, (char *)l->data);
            GtkWidget *row = build_guard_row((char *)l->data, full, TRUE);
            gtk_list_box_append(GTK_LIST_BOX(g_app.listbox_guards), row);
        }
        g_list_free_full(enabled_list, g_free);
    }

    GList *disabled_list = list_subdirs(g_disabled_guards_dir);
    for (GList *l = disabled_list; l; l = l->next) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", g_disabled_guards_dir, (char *)l->data);
        GtkWidget *row = build_guard_row((char *)l->data, full, FALSE);
        gtk_list_box_append(GTK_LIST_BOX(g_app.listbox_guards), row);
    }
    g_list_free_full(disabled_list, g_free);

    if (!have_addons) {
        GtkWidget *lbl = gtk_label_new(tr("Папка addons/ ещё не создана — запусти игру хотя бы раз.",
                                           "The addons/ folder doesn't exist yet — launch the game at least once."));
        gtk_widget_set_margin_top(lbl, 12);
        gtk_widget_set_margin_bottom(lbl, 12);
        gtk_list_box_append(GTK_LIST_BOX(g_app.listbox_guards), lbl);
    }
}

static void refresh_build_label(void) {
    char text[PATH_MAX + 64];
    if (g_file_test(g_current_link, G_FILE_TEST_EXISTS)) {
        char resolved[PATH_MAX];
        ssize_t n = readlink(g_current_link, resolved, sizeof(resolved) - 1);
        if (n > 0) {
            resolved[n] = '\0';
            snprintf(text, sizeof(text), "%s %s", tr("Активный билд:", "Active build:"), resolved);
        } else {
            snprintf(text, sizeof(text), "%s", tr("Активный билд: (ошибка чтения симлинка)", "Active build: (error reading symlink)"));
        }
    } else {
        snprintf(text, sizeof(text), "%s", tr("Активный билд: (не выбран)", "Active build: (not selected)"));
    }
    gtk_label_set_text(GTK_LABEL(g_app.label_build), text);
}

// ============================================================
// Импорт версии / мода / гварда — общий двухшаговый флоу:
// 1) выбрать папку-источник (нативный выбор папки)
// 2) ввести имя
// ============================================================

typedef enum { IMPORT_VERSION, IMPORT_MOD, IMPORT_GUARD } ImportKind;

typedef struct {
    ImportKind kind;
    char src_path[PATH_MAX];
} ImportCtx;

// --- Простое окно ввода текста (заменяет отсутствующий GtkEntryDialog) ---
typedef void (*TextEntryCb)(const char *text, gpointer user_data);

typedef struct {
    GtkWidget *win;
    GtkWidget *entry;
    TextEntryCb cb;
    gpointer user_data;
} TextEntryState;

static void text_entry_confirm(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TextEntryState *st = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(st->entry));
    char *copy = g_strdup(text);
    gtk_window_destroy(GTK_WINDOW(st->win));
    st->cb(copy, st->user_data);
    g_free(copy);
    g_free(st);
}

static void text_entry_cancel(GtkButton *btn, gpointer user_data) {
    (void)btn;
    TextEntryState *st = user_data;
    gtk_window_destroy(GTK_WINDOW(st->win));
    g_free(st);
}

static void ask_text(const char *title, const char *prompt, const char *default_text,
                      TextEntryCb cb, gpointer user_data) {
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(g_app.window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 420, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkWidget *lbl = gtk_label_new(prompt);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_box_append(GTK_BOX(box), lbl);

    GtkWidget *entry = gtk_entry_new();
    if (default_text) gtk_editable_set_text(GTK_EDITABLE(entry), default_text);
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *cancel_btn = gtk_button_new_with_label(btn_cancel_text());
    GtkWidget *ok_btn = gtk_button_new_with_label(tr("OK", "OK"));
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);
    gtk_box_append(GTK_BOX(box), btn_box);

    TextEntryState *st = g_new0(TextEntryState, 1);
    st->win = win;
    st->entry = entry;
    st->cb = cb;
    st->user_data = user_data;

    g_signal_connect(ok_btn, "clicked", G_CALLBACK(text_entry_confirm), st);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(text_entry_cancel), st);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_window_set_default_widget(GTK_WINDOW(win), ok_btn);

    gtk_window_present(GTK_WINDOW(win));
}

static void do_rsync_copy(const char *src, const char *dest) {
    ensure_dir(dest);
    char src_slash[PATH_MAX];
    snprintf(src_slash, sizeof(src_slash), "%s/", src);
    char dest_slash[PATH_MAX];
    snprintf(dest_slash, sizeof(dest_slash), "%s/", dest);

    char *src_q = g_shell_quote(src_slash);
    char *dest_q = g_shell_quote(dest_slash);
    char cmd[PATH_MAX * 2 + 64];
    snprintf(cmd, sizeof(cmd), "rsync -a %s %s", src_q, dest_q);
    int rc = run_cmd(cmd);
    g_free(src_q);
    g_free(dest_q);

    char msg[PATH_MAX + 64];
    if (rc == 0)
        snprintf(msg, sizeof(msg), "%s\n%s", tr("Готово! Скопировано в:", "Done! Copied to:"), dest);
    else
        snprintf(msg, sizeof(msg), tr("rsync завершился с ошибкой (код %d).", "rsync failed (exit code %d)."), rc);
    show_info(msg);
}

static void on_import_name_entered(const char *name, gpointer user_data) {
    ImportCtx *ctx = user_data;
    if (!name || !*name) {
        show_info(tr("Имя не может быть пустым — импорт отменён.", "Name cannot be empty — import cancelled."));
        g_free(ctx);
        return;
    }

    char dest[PATH_MAX];
    switch (ctx->kind) {
        case IMPORT_VERSION:
            snprintf(dest, sizeof(dest), "%s/%s", g_versions_dir, name);
            do_rsync_copy(ctx->src_path, dest);
            break;
        case IMPORT_MOD:
            snprintf(dest, sizeof(dest), "%s/%s", g_mods_dir, name);
            do_rsync_copy(ctx->src_path, dest);
            break;
        case IMPORT_GUARD: {
            char addons_dir[PATH_MAX];
            if (!find_guards_addon_dir(addons_dir, sizeof(addons_dir))) {
                show_info(tr("Папка addons/ не найдена в Wine-префиксе.\nЗапусти игру хотя бы раз.",
                              "The addons/ folder was not found in the Wine prefix.\nLaunch the game at least once."));
                g_free(ctx);
                return;
            }
            ensure_dir(addons_dir);
            snprintf(dest, sizeof(dest), "%s/%s", addons_dir, name);
            do_rsync_copy(ctx->src_path, dest);
            g_idle_add(idle_refresh_guards, NULL);
            break;
        }
    }
    g_free(ctx);
}

static void on_folder_chosen(GObject *source, GAsyncResult *res, gpointer user_data) {
    ImportCtx *ctx = user_data;
    GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
    GError *err = NULL;
    GFile *folder = gtk_file_dialog_select_folder_finish(dlg, res, &err);
    if (!folder) {
        if (err) g_error_free(err);
        g_free(ctx);
        return;
    }
    char *path = g_file_get_path(folder);
    g_strlcpy(ctx->src_path, path, sizeof(ctx->src_path));
    g_free(path);
    g_object_unref(folder);

    const char *prompt = NULL;
    const char *title = NULL;
    char default_name[256];
    char *base = g_path_get_basename(ctx->src_path);
    g_strlcpy(default_name, base, sizeof(default_name));
    g_free(base);

    switch (ctx->kind) {
        case IMPORT_VERSION:
            title = tr("Номер версии", "Version number");
            prompt = tr("Введи номер версии (например 1.1.4):", "Enter the version number (e.g. 1.1.4):");
            break;
        case IMPORT_MOD:
            title = tr("Имя мода", "Mod name");
            prompt = tr("Короткое имя мода:", "Short mod name:");
            break;
        case IMPORT_GUARD:
            title = tr("Имя гварда", "Guard name");
            prompt = tr("Имя гварда — так он будет называться В ИГРЕ:", "Guard name — this is how it will appear IN-GAME:");
            break;
    }
    ask_text(title, prompt, default_name, on_import_name_entered, ctx);
}

static void start_import(ImportKind kind) {
    ImportCtx *ctx = g_new0(ImportCtx, 1);
    ctx->kind = kind;

    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *title =
        kind == IMPORT_VERSION ? tr("Выбери ЧИСТУЮ (без мода) папку с игрой", "Select the CLEAN (unmodded) game folder") :
        kind == IMPORT_MOD     ? tr("Выбери папку мода", "Select the mod folder") :
                                  tr("Выбери папку custom guard", "Select the custom guard folder");
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(g_app.window), NULL, on_folder_chosen, ctx);
    g_object_unref(dlg);
}

static void on_import_version_clicked(GtkButton *b, gpointer d) { (void)b; (void)d; start_import(IMPORT_VERSION); }
static void on_import_mod_clicked(GtkButton *b, gpointer d)     { (void)b; (void)d; start_import(IMPORT_MOD); }
static void on_import_guard_clicked(GtkButton *b, gpointer d)   { (void)b; (void)d; start_import(IMPORT_GUARD); }

// ============================================================
// Сборка билда: версия (обязательно) + мод (опционально)
// ============================================================

typedef struct {
    GtkWidget *win;
    GtkWidget *combo_version;
    GtkWidget *combo_mod;
} BuildDialogState;

static void populate_combo(GtkWidget *combo, const char *dir, gboolean add_none) {
    if (add_none) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), tr("(без мода)", "(no mod)"));
    GList *items = list_subdirs(dir);
    for (GList *l = items; l; l = l->next) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), (char *)l->data);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_list_free_full(items, g_free);
}

static void activate_build(const char *build_path) {
    if (g_file_test(g_current_link, G_FILE_TEST_IS_SYMLINK) || g_file_test(g_current_link, G_FILE_TEST_EXISTS)) {
        g_unlink(g_current_link);
    }
    symlink(build_path, g_current_link);
    refresh_build_label();
}

static void on_build_confirmed(gboolean confirmed, gpointer user_data);

static void on_build_clicked_do(GtkButton *btn, gpointer user_data) {
    (void)btn;
    BuildDialogState *st = user_data;
    const char *ver = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(st->combo_version));
    const char *mod = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(st->combo_mod));

    if (!ver) {
        show_info(tr("Сначала импортируй хотя бы одну версию игры (пункт «Импорт версии»).",
                      "First import at least one game version (\"Import version\" button)."));
        return;
    }

    gboolean has_mod = mod && g_strcmp0(mod, tr("(без мода)", "(no mod)")) != 0;

    char build_name[300];
    if (has_mod)
        snprintf(build_name, sizeof(build_name), "%s-%s", ver, mod);
    else
        snprintf(build_name, sizeof(build_name), "%s-clean", ver);

    char build_path[PATH_MAX];
    snprintf(build_path, sizeof(build_path), "%s/%s", g_builds_dir, build_name);

    // Копируем данные, которые понадобятся после (возможного) подтверждения перезаписи
    char *ver_copy = g_strdup(ver);
    char *mod_copy = has_mod ? g_strdup(mod) : NULL;

    gtk_window_destroy(GTK_WINDOW(st->win));
    g_free(st);

    // структура для передачи в колбэк подтверждения
    typedef struct { char ver[256]; char mod[256]; gboolean has_mod; char build_name[300]; char build_path[PATH_MAX]; } PendingBuild;
    PendingBuild *pb = g_new0(PendingBuild, 1);
    g_strlcpy(pb->ver, ver_copy, sizeof(pb->ver));
    if (mod_copy) g_strlcpy(pb->mod, mod_copy, sizeof(pb->mod));
    pb->has_mod = has_mod;
    g_strlcpy(pb->build_name, build_name, sizeof(pb->build_name));
    g_strlcpy(pb->build_path, build_path, sizeof(pb->build_path));
    g_free(ver_copy);
    g_free(mod_copy);

    if (g_file_test(build_path, G_FILE_TEST_IS_DIR)) {
        char text[400];
        snprintf(text, sizeof(text),
                 tr("Сборка «%s» уже существует.\nПересобрать заново?",
                    "Build \"%s\" already exists.\nRebuild it?"),
                 build_name);
        ask_confirm(text, on_build_confirmed, pb);
    } else {
        on_build_confirmed(TRUE, pb);
    }
}

static void on_build_confirmed(gboolean confirmed, gpointer user_data) {
    typedef struct { char ver[256]; char mod[256]; gboolean has_mod; char build_name[300]; char build_path[PATH_MAX]; } PendingBuild;
    PendingBuild *pb = user_data;
    if (!confirmed) {
        // Пользователь отказался пересобирать — просто активируем существующую
        activate_build(pb->build_path);
        g_free(pb);
        return;
    }

    if (g_file_test(pb->build_path, G_FILE_TEST_IS_DIR)) {
        char *path_q = g_shell_quote(pb->build_path);
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", path_q);
        run_cmd(cmd);
        g_free(path_q);
    }
    ensure_dir(pb->build_path);

    char version_src[PATH_MAX];
    snprintf(version_src, sizeof(version_src), "%s/%s/", g_versions_dir, pb->ver);
    char *v_src_q = g_shell_quote(version_src);
    char *dest_q = g_shell_quote(pb->build_path);
    char cmd[PATH_MAX * 2 + 64];
    snprintf(cmd, sizeof(cmd), "rsync -a %s %s/", v_src_q, dest_q);
    run_cmd(cmd);
    g_free(v_src_q);

    if (pb->has_mod) {
        char mod_src[PATH_MAX];
        snprintf(mod_src, sizeof(mod_src), "%s/%s/", g_mods_dir, pb->mod);
        char *m_src_q = g_shell_quote(mod_src);
        snprintf(cmd, sizeof(cmd), "rsync -a %s %s/", m_src_q, dest_q);
        run_cmd(cmd);
        g_free(m_src_q);
    }
    g_free(dest_q);

    activate_build(pb->build_path);

    char msg[400];
    snprintf(msg, sizeof(msg), tr("Сборка «%s» готова и активирована.", "Build \"%s\" is ready and active."), pb->build_name);
    show_info(msg);
    g_free(pb);
}

static void on_build_cancel(GtkButton *btn, gpointer user_data) {
    (void)btn;
    BuildDialogState *st = user_data;
    gtk_window_destroy(GTK_WINDOW(st->win));
    g_free(st);
}

static void on_build_version_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), tr("Собрать билд", "Build"));
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(g_app.window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 380, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    gtk_box_append(GTK_BOX(box), gtk_label_new(tr("Версия игры:", "Game version:")));
    GtkWidget *combo_ver = gtk_combo_box_text_new();
    populate_combo(combo_ver, g_versions_dir, FALSE);
    gtk_box_append(GTK_BOX(box), combo_ver);

    gtk_box_append(GTK_BOX(box), gtk_label_new(tr("Мод (опционально):", "Mod (optional):")));
    GtkWidget *combo_mod = gtk_combo_box_text_new();
    populate_combo(combo_mod, g_mods_dir, TRUE);
    gtk_box_append(GTK_BOX(box), combo_mod);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *cancel_btn = gtk_button_new_with_label(btn_cancel_text());
    GtkWidget *ok_btn = gtk_button_new_with_label(tr("Собрать", "Build"));
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);
    gtk_box_append(GTK_BOX(box), btn_box);

    BuildDialogState *st = g_new0(BuildDialogState, 1);
    st->win = win;
    st->combo_version = combo_ver;
    st->combo_mod = combo_mod;

    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_build_clicked_do), st);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_build_cancel), st);

    gtk_window_present(GTK_WINDOW(win));
}

// ============================================================
// Переключение активного билда (откат)
// ============================================================

typedef struct {
    GtkWidget *win;
    GtkWidget *combo;
} SwitchDialogState;

static void on_switch_do(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SwitchDialogState *st = user_data;
    const char *build = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(st->combo));
    if (build) {
        char build_path[PATH_MAX];
        snprintf(build_path, sizeof(build_path), "%s/%s", g_builds_dir, build);
        activate_build(build_path);
        char msg[400];
        snprintf(msg, sizeof(msg), tr("Активная сборка переключена на:\n%s", "Active build switched to:\n%s"), build);
        show_info(msg);
    }
    gtk_window_destroy(GTK_WINDOW(st->win));
    g_free(st);
}

static void on_switch_cancel(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SwitchDialogState *st = user_data;
    gtk_window_destroy(GTK_WINDOW(st->win));
    g_free(st);
}

static void on_switch_build_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), tr("Переключить активную сборку", "Switch active build"));
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(g_app.window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 380, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    gtk_box_append(GTK_BOX(box), gtk_label_new(tr("Выбери сборку:", "Choose a build:")));
    GtkWidget *combo = gtk_combo_box_text_new();
    populate_combo(combo, g_builds_dir, FALSE);
    gtk_box_append(GTK_BOX(box), combo);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *cancel_btn = gtk_button_new_with_label(btn_cancel_text());
    GtkWidget *ok_btn = gtk_button_new_with_label(tr("Переключить", "Switch"));
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);
    gtk_box_append(GTK_BOX(box), btn_box);

    SwitchDialogState *st = g_new0(SwitchDialogState, 1);
    st->win = win;
    st->combo = combo;

    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_switch_do), st);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_switch_cancel), st);

    gtk_window_present(GTK_WINDOW(win));
}

// ============================================================
// Компиляция C-лаунчера + .desktop файл
// ============================================================

static void on_build_launcher_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;

    char tmp_c[] = "/tmp/frickbears3_launcher_XXXXXX.c";
    int fd = g_mkstemp(tmp_c);
    if (fd < 0) { show_info(tr("Не удалось создать временный файл.", "Failed to create a temporary file.")); return; }
    FILE *f = fdopen(fd, "w");
    fprintf(f,
        "#include <unistd.h>\n"
        "#define GAME_DIR \"%s\"\n"
        "#define GAME_EXE \"%s\"\n"
        "int main(void) {\n"
        "    chdir(GAME_DIR);\n"
        "    execlp(\"wine\", \"wine\", GAME_EXE, (char *)NULL);\n"
        "}\n",
        g_current_link, GAME_EXE);
    fclose(f);

    ensure_dir(g_bin_dir);
    char *tmp_q = g_shell_quote(tmp_c);
    char *out_q = g_shell_quote(g_launcher_path);
    char cmd[PATH_MAX * 2 + 64];
    snprintf(cmd, sizeof(cmd), "gcc -O2 -o %s %s", out_q, tmp_q);
    int rc = run_cmd(cmd);
    g_unlink(tmp_c);
    g_free(tmp_q);
    g_free(out_q);

    if (rc != 0) {
        show_info(tr("Ошибка компиляции. Проверь, установлен ли gcc.", "Compilation failed. Check that gcc is installed."));
        return;
    }
    chmod(g_launcher_path, 0755);

    // .desktop файл
    char apps_dir[PATH_MAX];
    snprintf(apps_dir, sizeof(apps_dir), "%s/.local/share/applications", g_home);
    ensure_dir(apps_dir);
    char desktop_path[PATH_MAX];
    snprintf(desktop_path, sizeof(desktop_path), "%s/frickbears3.desktop", apps_dir);
    FILE *df = fopen(desktop_path, "w");
    if (df) {
        fprintf(df,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Five Nights at Frickbear's 3\n"
            "Comment=%s\n"
            "Exec=%s\n"
            "Icon=applications-games\n"
            "Categories=Game;\n"
            "Terminal=false\n",
            tr("Запуск игры через Wine", "Launch the game via Wine"),
            g_launcher_path);
        fclose(df);
    }

    char msg[600];
    snprintf(msg, sizeof(msg),
             tr("Лаунчер скомпилирован:\n%s\n\nЯрлык добавлен в меню приложений.\n"
                "Пересобирать не нужно при смене версии/мода — только если сменится имя exe.",
                "Launcher compiled:\n%s\n\nA shortcut was added to the application menu.\n"
                "No need to rebuild after changing version/mod — only if the exe name changes."),
             g_launcher_path);
    show_info(msg);
}

// ============================================================
// Очистка старых версий/модов/билдов
// ============================================================

typedef struct {
    GtkWidget *win;
    GtkWidget *combo_category;
    GtkWidget *combo_item;
} CleanupState;

static const char *cleanup_dir_for_category(int idx) {
    if (idx == 0) return g_versions_dir;
    if (idx == 1) return g_mods_dir;
    return g_builds_dir;
}

static void on_cleanup_category_changed(GtkComboBox *combo, gpointer user_data) {
    CleanupState *st = user_data;
    int idx = gtk_combo_box_get_active(combo);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(st->combo_item));
    populate_combo(st->combo_item, cleanup_dir_for_category(idx), FALSE);
}

static void on_cleanup_delete_confirmed(gboolean confirmed, gpointer user_data) {
    char *full_path = user_data;
    if (confirmed) {
        char *q = g_shell_quote(full_path);
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", q);
        run_cmd(cmd);
        g_free(q);
        show_info(tr("Удалено.", "Deleted."));
    }
    g_free(full_path);
}

static void on_cleanup_delete_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    CleanupState *st = user_data;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(st->combo_category));
    const char *item = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(st->combo_item));
    if (!item) { show_info(tr("Нечего удалять — список пуст.", "Nothing to delete — the list is empty.")); return; }

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", cleanup_dir_for_category(idx), item);

    char text[PATH_MAX + 64];
    snprintf(text, sizeof(text), tr("Удалить безвозвратно:\n%s ?", "Permanently delete:\n%s ?"), full_path);
    ask_confirm(text, on_cleanup_delete_confirmed, g_strdup(full_path));
}

static void on_cleanup_close(GtkButton *btn, gpointer user_data) {
    (void)btn;
    CleanupState *st = user_data;
    gtk_window_destroy(GTK_WINDOW(st->win));
    g_free(st);
}

static void on_cleanup_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), tr("Очистка", "Cleanup"));
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(g_app.window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 380, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    gtk_box_append(GTK_BOX(box), gtk_label_new(tr("Категория:", "Category:")));
    GtkWidget *combo_cat = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cat), tr("Версии", "Versions"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cat), tr("Моды", "Mods"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_cat), tr("Сборки", "Builds"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_cat), 0);
    gtk_box_append(GTK_BOX(box), combo_cat);

    gtk_box_append(GTK_BOX(box), gtk_label_new(tr("Что удалить:", "What to delete:")));
    GtkWidget *combo_item = gtk_combo_box_text_new();
    populate_combo(combo_item, g_versions_dir, FALSE);
    gtk_box_append(GTK_BOX(box), combo_item);

    CleanupState *st = g_new0(CleanupState, 1);
    st->win = win;
    st->combo_category = combo_cat;
    st->combo_item = combo_item;

    g_signal_connect(combo_cat, "changed", G_CALLBACK(on_cleanup_category_changed), st);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *close_btn = gtk_button_new_with_label(tr("Закрыть", "Close"));
    GtkWidget *del_btn = gtk_button_new_with_label(tr("Удалить", "Delete"));
    gtk_widget_add_css_class(del_btn, "destructive-action");
    gtk_box_append(GTK_BOX(btn_box), close_btn);
    gtk_box_append(GTK_BOX(btn_box), del_btn);
    gtk_box_append(GTK_BOX(box), btn_box);

    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_cleanup_delete_clicked), st);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_cleanup_close), st);

    gtk_window_present(GTK_WINDOW(win));
}

// ============================================================
// Кнопка "Обновить список"
// ============================================================

static void on_refresh_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    refresh_guards_list();
    refresh_build_label();
}

// ============================================================
// Построение главного окна
// ============================================================

static void activate(GtkApplication *app, gpointer user_data);

static void on_lang_toggle_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(g_app.window));
    lang_set(lang_get() == LANG_RU ? LANG_EN : LANG_RU);
    gtk_window_destroy(GTK_WINDOW(g_app.window));
    activate(app, NULL);
}

static void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    g_app.window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(g_app.window), "Frickbears 3 Manager");
    gtk_window_set_default_size(GTK_WINDOW(g_app.window), 560, 640);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 14);
    gtk_widget_set_margin_end(root, 14);
    gtk_widget_set_margin_top(root, 14);
    gtk_widget_set_margin_bottom(root, 14);
    gtk_window_set_child(GTK_WINDOW(g_app.window), root);

    // Строка активного билда + кнопка переключения языка
    GtkWidget *top_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g_app.label_build = gtk_label_new(tr("Активный билд: (загрузка...)", "Active build: (loading...)"));
    gtk_widget_set_hexpand(g_app.label_build, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_app.label_build), 0.0);
    gtk_widget_add_css_class(g_app.label_build, "heading");
    gtk_box_append(GTK_BOX(top_row), g_app.label_build);

    GtkWidget *lang_btn = gtk_button_new_with_label(lang_get() == LANG_RU ? "EN" : "RU");
    gtk_widget_set_tooltip_text(lang_btn, tr("Переключить язык", "Switch language"));
    g_signal_connect(lang_btn, "clicked", G_CALLBACK(on_lang_toggle_clicked), NULL);
    gtk_box_append(GTK_BOX(top_row), lang_btn);
    gtk_box_append(GTK_BOX(root), top_row);

    // Панель кнопок (две строки flow-box'ом через обычные box'ы)
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(root), row1);
    gtk_box_append(GTK_BOX(root), row2);

    struct { const char *label; GCallback cb; } row1_btns[] = {
        {tr("Импорт версии", "Import version"), G_CALLBACK(on_import_version_clicked)},
        {tr("Импорт мода",   "Import mod"),     G_CALLBACK(on_import_mod_clicked)},
        {tr("Импорт гварда", "Import guard"),   G_CALLBACK(on_import_guard_clicked)},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(row1_btns); i++) {
        GtkWidget *btn = gtk_button_new_with_label(row1_btns[i].label);
        gtk_widget_set_hexpand(btn, TRUE);
        g_signal_connect(btn, "clicked", row1_btns[i].cb, NULL);
        gtk_box_append(GTK_BOX(row1), btn);
    }

    struct { const char *label; GCallback cb; } row2_btns[] = {
        {tr("Собрать билд",    "Build"),         G_CALLBACK(on_build_version_clicked)},
        {tr("Свитч билда",     "Switch build"),  G_CALLBACK(on_switch_build_clicked)},
        {tr("Собрать лаунчер", "Build launcher"),G_CALLBACK(on_build_launcher_clicked)},
        {tr("Очистка",         "Cleanup"),       G_CALLBACK(on_cleanup_clicked)},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(row2_btns); i++) {
        GtkWidget *btn = gtk_button_new_with_label(row2_btns[i].label);
        gtk_widget_set_hexpand(btn, TRUE);
        g_signal_connect(btn, "clicked", row2_btns[i].cb, NULL);
        gtk_box_append(GTK_BOX(row2), btn);
    }

    // Заголовок списка + кнопка обновить
    GtkWidget *guards_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *guards_label = gtk_label_new(tr("Custom guards (addons/):", "Custom guards (addons/):"));
    gtk_widget_set_hexpand(guards_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(guards_label), 0.0);
    gtk_widget_add_css_class(guards_label, "heading");
    GtkWidget *refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_btn, tr("Обновить список", "Refresh list"));
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_box_append(GTK_BOX(guards_header), guards_label);
    gtk_box_append(GTK_BOX(guards_header), refresh_btn);
    gtk_box_append(GTK_BOX(root), guards_header);

    // Скроллящийся список гвардов
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    g_app.listbox_guards = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_app.listbox_guards), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), g_app.listbox_guards);
    gtk_box_append(GTK_BOX(root), scroll);

    refresh_guards_list();
    refresh_build_label();

    gtk_window_present(GTK_WINDOW(g_app.window));
}

// ============================================================
// main()
// ============================================================

int main(int argc, char **argv) {
    g_strlcpy(g_home, g_get_home_dir(), sizeof(g_home));
    snprintf(g_base, sizeof(g_base), "%s/.local/share/frickbears3", g_home);
    snprintf(g_versions_dir, sizeof(g_versions_dir), "%s/versions", g_base);
    snprintf(g_mods_dir, sizeof(g_mods_dir), "%s/mods", g_base);
    snprintf(g_builds_dir, sizeof(g_builds_dir), "%s/builds", g_base);
    snprintf(g_current_link, sizeof(g_current_link), "%s/current", g_base);
    snprintf(g_disabled_guards_dir, sizeof(g_disabled_guards_dir), "%s/guards_disabled", g_base);
    snprintf(g_bin_dir, sizeof(g_bin_dir), "%s/.local/bin", g_home);
    snprintf(g_launcher_path, sizeof(g_launcher_path), "%s/frickbears3", g_bin_dir);

    ensure_dir(g_versions_dir);
    ensure_dir(g_mods_dir);
    ensure_dir(g_builds_dir);
    ensure_dir(g_disabled_guards_dir);
    ensure_dir(g_bin_dir);
    
    const char *lang = g_getenv("LANG");

    if (lang && g_str_has_prefix(lang, "en")) {
        lang_set(LANG_EN);
    } else {
        lang_set(LANG_RU);
    }

    GtkApplication *app = gtk_application_new("com.aleksik.frickbears3manager", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
