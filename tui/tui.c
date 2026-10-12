#include "core/main.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define EDITOR_CAPACITY 65536
#define PREVIEW_CAPACITY 4096
#define KEY_SHIFT_ENTER (KEY_MAX + 1)

typedef struct {
    char *name;
    int is_directory;
    off_t size;
} file_entry;

typedef struct {
    file_entry *items;
    size_t count;
    size_t capacity;
} file_list;

static int entry_compare(const void *left, const void *right) {
    const file_entry *a = (const file_entry *)left;
    const file_entry *b = (const file_entry *)right;
    if (a->is_directory != b->is_directory) {
        return b->is_directory - a->is_directory;
    }
    return strcasecmp(a->name, b->name);
}

static void free_entries(file_list *list) {
    size_t i;
    for (i = 0; i < list->count; ++i) free(list->items[i].name);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int add_entry(file_list *list, const char *name, int is_directory, off_t size) {
    file_entry *grown;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 32 : list->capacity * 2;
        grown = (file_entry *)realloc(list->items, capacity * sizeof(*grown));
        if (grown == NULL) return -1;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count].name = strdup(name);
    if (list->items[list->count].name == NULL) return -1;
    list->items[list->count].is_directory = is_directory;
    list->items[list->count].size = size;
    ++list->count;
    return 0;
}

static int load_entries(const char *path, file_list *list) {
    DIR *directory;
    struct dirent *entry;
    free_entries(list);
    if (add_entry(list, "..", 1, 0) != 0) return -1;
    directory = opendir(path);
    if (directory == NULL) {
        free_entries(list);
        return -1;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char full_path[PATH_MAX];
        struct stat info;
        int is_directory = 0;
        off_t size = 0;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name)
            >= (int)sizeof(full_path)) continue;
        if (stat(full_path, &info) == 0) {
            is_directory = S_ISDIR(info.st_mode);
            size = info.st_size;
        }
        if (add_entry(list, entry->d_name, is_directory, size) != 0) {
            int saved_errno = errno;
            closedir(directory);
            free_entries(list);
            errno = saved_errno;
            return -1;
        }
        errno = 0;
    }
    if (errno != 0) {
        int saved_errno = errno;
        closedir(directory);
        free_entries(list);
        errno = saved_errno;
        return -1;
    }
    if (closedir(directory) == -1) {
        free_entries(list);
        return -1;
    }
    qsort(list->items + 1, list->count - 1, sizeof(*list->items), entry_compare);
    return 0;
}

static int matches_query(const char *name, const char *query) {
    const char *start;
    if (query[0] == '\0') return 1;
    for (start = name; *start != '\0'; ++start) {
        const char *left = start;
        const char *right = query;
        while (*left != '\0' && *right != '\0'
               && tolower((unsigned char)*left) == tolower((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') return 1;
    }
    return 0;
}

static size_t visible_count(const file_list *list, const char *query) {
    size_t i;
    size_t count = 0;
    for (i = 0; i < list->count; ++i) {
        if (matches_query(list->items[i].name, query)) ++count;
    }
    return count;
}

static size_t visible_index(const file_list *list, const char *query, size_t wanted) {
    size_t i;
    size_t current = 0;
    for (i = 0; i < list->count; ++i) {
        if (matches_query(list->items[i].name, query) && current++ == wanted) return i;
    }
    return list->count;
}

static void draw_safe_text(WINDOW *window, int y, int x, const char *text, int max_width) {
    int column = 0;
    const unsigned char *p = (const unsigned char *)text;
    wmove(window, y, x);
    while (*p != '\0' && column < max_width) {
        waddch(window, *p >= 0x20 && *p != 0x7f ? *p : '?');
        ++column;
        ++p;
    }
}

static void draw_browser(const char *path, const file_list *list, const char *query,
                         size_t *selected, const char *message) {
    int height;
    int width;
    int list_width;
    int content_height;
    int rows_per_page;
    int start;
    int i;
    int shown = 0;
    size_t actual_selected;
    WINDOW *left;
    WINDOW *right;

    getmaxyx(stdscr, height, width);
    erase();
    if (height < 12 || width < 60) {
        mvaddstr(0, 0, "Resize terminal to at least 60 columns by 12 rows.");
        mvaddstr(2, 0, "Press q to quit.");
        refresh();
        return;
    }
    list_width = width * 2 / 5;
    if (list_width < 24) list_width = 24;
    if (list_width > width - 24) list_width = width - 24;
    content_height = height - 4;
    rows_per_page = content_height - 2;
    actual_selected = visible_index(list, query, *selected);
    if (actual_selected == list->count) {
        *selected = 0;
        actual_selected = visible_index(list, query, *selected);
    }
    start = *selected >= (size_t)rows_per_page
        ? (int)(*selected - (size_t)rows_per_page + 1) : 0;

    attron(A_BOLD);
    mvaddnstr(0, 0, "TExplorer", width - 1);
    attroff(A_BOLD);
    mvaddnstr(0, 12, path, width - 13);
    left = newwin(content_height, list_width, 1, 0);
    right = newwin(content_height, width - list_width, 1, list_width);
    if (left == NULL || right == NULL) {
        if (left != NULL) delwin(left);
        if (right != NULL) delwin(right);
        mvaddstr(2, 0, "Unable to allocate terminal panels.");
        refresh();
        return;
    }
    box(left, 0, 0);
    box(right, 0, 0);
    mvwaddnstr(left, 0, 2, query[0] ? " Files: filtered " : " Files ", list_width - 4);
    mvwaddstr(right, 0, 2, " Preview ");

    for (i = 0; (size_t)i < list->count && shown < rows_per_page; ++i) {
        const file_entry *item = &list->items[i];
        char label[PATH_MAX];
        int row;
        if (!matches_query(item->name, query)) continue;
        if (shown < start) {
            ++shown;
            continue;
        }
        row = shown - start + 1;
        snprintf(label, sizeof(label), "%s%s", item->name, item->is_directory ? "/" : "");
        if ((size_t)shown == *selected) wattron(left, A_REVERSE);
        draw_safe_text(left, row, 1, label, list_width - 3);
        if ((size_t)shown == *selected) wattroff(left, A_REVERSE);
        ++shown;
    }

    if (actual_selected < list->count) {
        const file_entry *item = &list->items[actual_selected];
        char full_path[PATH_MAX];
        struct stat info;
        if (snprintf(full_path, sizeof(full_path), "%s/%s", path, item->name)
            < (int)sizeof(full_path)) {
            if (item->is_directory) {
                mvwaddstr(right, 2, 2, "Directory");
                mvwaddstr(right, 4, 2, "Enter to open this folder");
            } else if (stat(full_path, &info) == 0 && S_ISREG(info.st_mode)) {
                FILE *file = fopen(full_path, "rb");
                if (file != NULL) {
                    char preview[PREVIEW_CAPACITY];
                    size_t count = fread(preview, 1, sizeof(preview) - 1, file);
                    int row = 2;
                    int column = 0;
                    size_t j;
                    mvwprintw(right, row++, 2, "File: %s", item->name);
                    mvwprintw(right, row++, 2, "Size: %lld bytes", (long long)info.st_size);
                    mvwaddstr(right, row++, 2, "Preview:");
                    for (j = 0; j < count && row < content_height - 1; ++j) {
                        unsigned char ch = (unsigned char)preview[j];
                        if (ch == '\n') {
                            ++row;
                            column = 0;
                        } else if (ch != '\r') {
                            if (column >= width - list_width - 4) {
                                ++row;
                                column = 0;
                            }
                            if (row < content_height - 1) {
                                mvwaddch(right, row, 2 + column,
                                         isprint(ch) || ch == '\t' ? ch : '?');
                            }
                            ++column;
                        }
                    }
                    if (ferror(file)) mvwaddstr(right, row, 2, "Error reading preview.");
                    fclose(file);
                } else {
                    mvwprintw(right, 2, 2, "Cannot read file: %s", strerror(errno));
                }
            } else {
                mvwaddstr(right, 2, 2, "File or special item");
            }
        }
    }

    mvhline(height - 3, 0, ACS_HLINE, width);
    if (query[0] != '\0') {
        mvaddnstr(height - 2, 0, "Search: ", width - 1);
        mvaddnstr(height - 2, 8, query, width - 9);
    } else if (message[0] != '\0') {
        mvaddnstr(height - 2, 0, message, width - 1);
    } else {
        mvaddnstr(height - 2, 0,
                  "Enter open  / search  e edit  n new  Shift+Enter menu  : shell  q quit",
                  width - 1);
    }
    mvaddnstr(height - 1, 0, "Backspace: parent  r: refresh", width - 1);
    wnoutrefresh(stdscr);
    wnoutrefresh(left);
    wnoutrefresh(right);
    doupdate();
    delwin(left);
    delwin(right);
}

static int prompt_text(const char *label, char *buffer, size_t capacity) {
    int height;
    int width;
    int result;
    getmaxyx(stdscr, height, width);
    move(height - 2, 0);
    clrtoeol();
    addnstr(label, width - 1);
    refresh();
    echo();
    curs_set(1);
    result = getnstr(buffer, (int)capacity - 1);
    noecho();
    curs_set(0);
    return result == ERR ? -1 : 0;
}

static int confirm_action(const char *question) {
    char answer[8] = "";
    if (prompt_text(question, answer, sizeof(answer)) != 0) return 0;
    return answer[0] == 'y' || answer[0] == 'Y';
}

static int build_path(char *result, size_t capacity, const char *directory,
                      const char *name) {
    int written = snprintf(result, capacity, "%s/%s", directory, name);
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

static int change_directory(char *path, size_t capacity, const char *target) {
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    int written = snprintf(candidate, sizeof(candidate), "%s/%s", path, target);
    if (written < 0 || (size_t)written >= sizeof(candidate)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (realpath(candidate, resolved) == NULL) return -1;
    if (strlen(resolved) >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(path, resolved);
    return 0;
}

static int view_file(const char *path) {
    FILE *file = fopen(path, "rb");
    char *contents;
    size_t count;
    size_t offset = 0;
    int height;
    int width;
    int line = 2;
    if (file == NULL) return -1;
    contents = (char *)malloc(EDITOR_CAPACITY);
    if (contents == NULL) {
        fclose(file);
        errno = ENOMEM;
        return -1;
    }
    count = fread(contents, 1, EDITOR_CAPACITY - 1, file);
    contents[count] = '\0';
    getmaxyx(stdscr, height, width);
    erase();
    attron(A_BOLD);
    mvaddnstr(0, 0, path, width - 1);
    attroff(A_BOLD);
    mvhline(1, 0, ACS_HLINE, width);
    while (offset < count && line < height - 2) {
        size_t start = offset;
        size_t end;
        while (offset < count && contents[offset] != '\n') ++offset;
        end = offset - start;
        if (end > (size_t)(width - 1)) end = (size_t)(width - 1);
        if (end > 0) {
            char saved = contents[start + end];
            contents[start + end] = '\0';
            draw_safe_text(stdscr, line++, 0, contents + start, (int)end);
            contents[start + end] = saved;
        } else {
            ++line;
        }
        if (offset < count && contents[offset] == '\n') ++offset;
    }
    mvaddstr(height - 2, 0,
             count == EDITOR_CAPACITY - 1
                 ? "Read-only view (first 64 KiB). Press any key to return."
                 : "Read-only view. Press any key to return.");
    refresh();
    getch();
    free(contents);
    if (ferror(file)) {
        fclose(file);
        errno = EIO;
        return -1;
    }
    if (fclose(file) == EOF) return -1;
    return 0;
}

static size_t editor_line_start(const char *text, size_t cursor) {
    while (cursor > 0 && text[cursor - 1] != '\n') --cursor;
    return cursor;
}

static size_t editor_line_end(const char *text, size_t length, size_t cursor) {
    while (cursor < length && text[cursor] != '\n') ++cursor;
    return cursor;
}

static int read_tui_key(void) {
    int key = getch();
    if (key != 27) return key;

    set_escdelay(25);
    {
        int next = getch();
        if (next == ERR) return key;
        if (next != '[') {
            ungetch(next);
            return key;
        }
        next = getch();
        if (next == ERR) return key;
        switch (next) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            default: ungetch(next); return key;
        }
    }
}

static void draw_editor(const char *path, const char *text, size_t length, size_t cursor,
                        int modified, const char *message) {
    int height;
    int width;
    int cursor_row = 0;
    int cursor_column = 0;
    int row = 2;
    int top_row;
    size_t i;
    size_t start = 0;
    size_t line = 0;
    getmaxyx(stdscr, height, width);
    for (i = 0; i < cursor; ++i) {
        if (text[i] == '\n') {
            ++cursor_row;
            cursor_column = 0;
        } else {
            ++cursor_column;
        }
    }
    top_row = cursor_row > height - 5 ? cursor_row - (height - 5) : 0;
    while (line < (size_t)top_row && start < length) {
        while (start < length && text[start++] != '\n') {}
        ++line;
    }
    erase();
    attron(A_BOLD);
    mvaddnstr(0, 0, path, width - 1);
    if (modified && width > 12) mvaddstr(0, width - 12, "[modified]");
    attroff(A_BOLD);
    mvhline(1, 0, ACS_HLINE, width);
    i = start;
    while (i <= length && row < height - 2) {
        int column = 0;
        while (i < length && text[i] != '\n' && column < width - 1) {
            unsigned char ch = (unsigned char)text[i++];
            mvaddch(row, column++, ch >= 0x20 && ch != 0x7f ? ch : '?');
        }
        ++row;
        if (i < length && text[i] == '\n') ++i;
        else if (i >= length) break;
    }
    mvhline(height - 2, 0, ACS_HLINE, width);
    mvaddnstr(height - 1, 0,
              message[0] != '\0' ? message
                                 : "Ctrl+S save  Esc quit  arrows move  Backspace erase",
              width - 1);
    move(cursor_row - top_row + 2, cursor_column < width ? cursor_column : width - 1);
    refresh();
}

static int save_editor(const char *path, const char *text, size_t length) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return -1;
    if (fwrite(text, 1, length, file) != length) {
        int saved_errno = errno;
        fclose(file);
        errno = saved_errno == 0 ? EIO : saved_errno;
        return -1;
    }
    return fclose(file) == EOF ? -1 : 0;
}

static int edit_file(const char *path) {
    FILE *file = fopen(path, "rb");
    char *text = (char *)malloc(EDITOR_CAPACITY);
    size_t length = 0;
    size_t cursor = 0;
    int modified = 0;
    int done = 0;
    char message[128] = "";
    if (text == NULL) return -1;
    if (file != NULL) {
        length = fread(text, 1, EDITOR_CAPACITY, file);
        if (ferror(file) || length == EDITOR_CAPACITY) {
            int saved_errno = ferror(file) ? errno : EFBIG;
            fclose(file);
            free(text);
            errno = saved_errno;
            return -1;
        }
        if (fclose(file) == EOF) {
            free(text);
            return -1;
        }
    } else if (errno != ENOENT) {
        free(text);
        return -1;
    }
    while (!done) {
        int key;
        size_t line_start;
        size_t line_end;
        draw_editor(path, text, length, cursor, modified, message);
        key = read_tui_key();
        line_start = editor_line_start(text, cursor);
        line_end = editor_line_end(text, length, cursor);
        if (key == 19) {
            if (save_editor(path, text, length) == 0) {
                modified = 0;
                snprintf(message, sizeof(message), "Saved.");
            } else {
                snprintf(message, sizeof(message), "Save failed: %s", strerror(errno));
            }
        } else if (key == 27) {
            if (!modified || confirm_action("Save changes before exit? (y/N) ")) {
                if (modified && save_editor(path, text, length) != 0) {
                    snprintf(message, sizeof(message), "Save failed: %s", strerror(errno));
                } else {
                    done = 1;
                }
            } else {
                done = 1;
            }
        } else if ((key == KEY_BACKSPACE || key == 127 || key == 8) && cursor > 0) {
            memmove(text + cursor - 1, text + cursor, length - cursor);
            --cursor;
            --length;
            modified = 1;
        } else if (key == KEY_DC && cursor < length) {
            memmove(text + cursor, text + cursor + 1, length - cursor - 1);
            --length;
            modified = 1;
        } else if (key == KEY_LEFT && cursor > 0) {
            --cursor;
        } else if (key == KEY_RIGHT && cursor < length) {
            ++cursor;
        } else if (key == KEY_HOME) {
            cursor = line_start;
        } else if (key == KEY_END) {
            cursor = line_end;
        } else if (key == KEY_UP || key == KEY_DOWN) {
            size_t column = cursor - line_start;
            size_t target;
            if (key == KEY_UP) {
                if (line_start == 0) continue;
                target = editor_line_start(text, line_start - 1);
            } else {
                if (line_end >= length) continue;
                target = line_end + 1;
            }
            {
                size_t target_end = editor_line_end(text, length, target);
                size_t target_length = target_end - target;
                cursor = target + (column < target_length ? column : target_length);
            }
        } else if ((key == '\n' || key == '\r' || key == KEY_ENTER)
                   && length + 1 < EDITOR_CAPACITY) {
            memmove(text + cursor + 1, text + cursor, length - cursor);
            text[cursor++] = '\n';
            ++length;
            modified = 1;
        } else if (key >= 0x20 && key <= 0xff && length + 1 < EDITOR_CAPACITY) {
            memmove(text + cursor + 1, text + cursor, length - cursor);
            text[cursor++] = (char)key;
            ++length;
            modified = 1;
        }
    }
    free(text);
    return 0;
}

static int create_and_edit(const char *directory) {
    char name[256] = "";
    char path[PATH_MAX];
    int descriptor;
    if (prompt_text("New file name: ", name, sizeof(name)) != 0
        || name[0] == '\0' || strchr(name, '/') != NULL
        || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (build_path(path, sizeof(path), directory, name) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (descriptor == -1) return -1;
    if (close(descriptor) == -1) return -1;
    return edit_file(path);
}

static void run_shell_command(const char *current_directory, char *message,
                              size_t message_capacity) {
    char command[512] = "";
    char old_directory[PATH_MAX];
    int status;
    int shell_status;
    int shell_error;
    int exited;
    int exit_code;
    if (prompt_text("Shell command (runs in current directory): ", command,
                    sizeof(command)) != 0 || command[0] == '\0') return;
    if (getcwd(old_directory, sizeof(old_directory)) == NULL
        || chdir(current_directory) != 0) {
        snprintf(message, message_capacity, "Cannot enter directory: %s", strerror(errno));
        return;
    }
    endwin();
    printf("\n[TExplorer shell] %s\n", command);
    fflush(stdout);
    status = system(command);
    shell_error = status == -1;
    exited = !shell_error && WIFEXITED(status);
    exit_code = exited ? WEXITSTATUS(status) : -1;
    shell_status = exited ? exit_code : status;
    printf("\n[command finished; %s %d] Press Enter to return.",
           shell_error ? "could not start" : exited ? "exit status" : "status",
           shell_status);
    fflush(stdout);
    while (getchar() != '\n' && !feof(stdin)) {}
    if (chdir(old_directory) != 0) {
        snprintf(message, message_capacity, "Cannot restore working directory: %s",
                 strerror(errno));
    }
    refresh();
}

static int context_menu(void) {
    static const char *choices[] = {"Open / enter", "Read-only view", "Edit file",
                                    "Create a new file", "Cancel"};
    int selected = 0;
    int height;
    int width;
    WINDOW *menu;
    getmaxyx(stdscr, height, width);
    if (height < 10 || width < 34) return 4;
    menu = newwin(9, 32, (height - 9) / 2, (width - 32) / 2);
    if (menu == NULL) return 4;
    keypad(menu, TRUE);
    while (1) {
        int i;
        int key;
        werase(menu);
        box(menu, 0, 0);
        mvwaddstr(menu, 1, 2, "Context menu");
        for (i = 0; i < 5; ++i) {
            if (selected == i) wattron(menu, A_REVERSE);
            mvwaddstr(menu, i + 3, 2, choices[i]);
            if (selected == i) wattroff(menu, A_REVERSE);
        }
        wrefresh(menu);
        key = wgetch(menu);
        if (key == KEY_UP) selected = (selected + 4) % 5;
        else if (key == KEY_DOWN) selected = (selected + 1) % 5;
        else if (key == 27 || key == 'q') {
            selected = 4;
            break;
        }
        else if (key == '\n' || key == '\r' || key == KEY_ENTER) break;
    }
    delwin(menu);
    return selected;
}

int run_tui(const char *initial_path) {
    char path[PATH_MAX];
    char resolved[PATH_MAX];
    char query[256] = "";
    char message[256] = "";
    file_list list = {0};
    size_t selected = 0;
    int running = 1;

    if (realpath(initial_path, resolved) == NULL) {
        fprintf(stderr, "Cannot open directory '%s': %s\n", initial_path, strerror(errno));
        return 1;
    }
    if (strlen(resolved) >= sizeof(path)) {
        fprintf(stderr, "Directory path is too long: '%s'\n", initial_path);
        return 1;
    }
    strcpy(path, resolved);
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "The interactive file manager requires a terminal.\n");
        return 1;
    }
    if (load_entries(path, &list) != 0) {
        fprintf(stderr, "Cannot read directory '%s': %s\n", path, strerror(errno));
        return 1;
    }
    initscr();
    set_escdelay(25);
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    mousemask(ALL_MOUSE_EVENTS, NULL);
    define_key("\033[13;2u", KEY_SHIFT_ENTER);
    fputs("\033[>1u", stdout);
    fflush(stdout);

    while (running) {
        int key;
        size_t count;
        size_t actual;
        draw_browser(path, &list, query, &selected, message);
        message[0] = '\0';
        key = read_tui_key();
        count = visible_count(&list, query);
        if (key == 'q' || key == 'Q') {
            running = 0;
        } else if (key == KEY_UP && selected > 0) {
            --selected;
        } else if (key == KEY_DOWN && selected + 1 < count) {
            ++selected;
        } else if (key == KEY_PPAGE) {
            selected = selected > 10 ? selected - 10 : 0;
        } else if (key == KEY_NPAGE) {
            selected = selected + 10 < count ? selected + 10 : (count == 0 ? 0 : count - 1);
        } else if (key == '/') {
            if (prompt_text("Search current directory: ", query, sizeof(query)) != 0) {
                query[0] = '\0';
            }
            selected = 0;
        } else if (key == 27 && query[0] != '\0') {
            query[0] = '\0';
            selected = 0;
        } else if (key == 'r') {
            if (load_entries(path, &list) != 0) {
                snprintf(message, sizeof(message), "Refresh failed: %s", strerror(errno));
            }
        } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (strcmp(path, "/") != 0 && change_directory(path, sizeof(path), "..") == 0) {
                selected = 0;
                query[0] = '\0';
                if (load_entries(path, &list) != 0) {
                    snprintf(message, sizeof(message), "Cannot read directory: %s",
                             strerror(errno));
                }
            }
        } else if (key == ':') {
            run_shell_command(path, message, sizeof(message));
            clear();
            refresh();
            if (load_entries(path, &list) != 0) {
                snprintf(message, sizeof(message), "Refresh failed: %s", strerror(errno));
            }
        } else if (key == 'n') {
            if (create_and_edit(path) != 0) {
                snprintf(message, sizeof(message), "Create/edit failed: %s", strerror(errno));
            }
            if (load_entries(path, &list) != 0) {
                snprintf(message, sizeof(message), "Refresh failed: %s", strerror(errno));
            }
        } else if (key == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK && (event.bstate & BUTTON3_PRESSED) != 0) {
                int height;
                int width;
                int list_width;
                int rows_per_page;
                int page_start;
                int mouse_index;
                int visible = 0;
                size_t i;
                getmaxyx(stdscr, height, width);
                list_width = width * 2 / 5;
                if (list_width < 24) list_width = 24;
                if (list_width > width - 24) list_width = width - 24;
                rows_per_page = height - 6;
                page_start = selected >= (size_t)rows_per_page
                    ? (int)(selected - (size_t)rows_per_page + 1) : 0;
                mouse_index = page_start + event.y - 2;
                if (event.x < list_width && event.y >= 2 && event.y < height - 3) {
                    for (i = 0; i < list.count; ++i) {
                        if (!matches_query(list.items[i].name, query)) continue;
                        if (visible == mouse_index) {
                            selected = (size_t)mouse_index;
                            break;
                        }
                        ++visible;
                    }
                }
                key = KEY_SHIFT_ENTER;
            } else {
                continue;
            }
        }

        if (key == KEY_SHIFT_ENTER || key == '\n' || key == '\r' || key == KEY_ENTER) {
            actual = visible_index(&list, query, selected);
            if (actual >= list.count) continue;
            if (key == KEY_SHIFT_ENTER) {
                int action = context_menu();
                if (action == 4) continue;
                if (action == 3) {
                    if (create_and_edit(path) != 0) {
                        snprintf(message, sizeof(message), "Create/edit failed: %s",
                                 strerror(errno));
                    }
                    if (load_entries(path, &list) != 0) {
                        snprintf(message, sizeof(message), "Refresh failed: %s",
                                 strerror(errno));
                    }
                    continue;
                }
                if (action == 1 || action == 2) {
                    char full_path[PATH_MAX];
                    if (list.items[actual].is_directory
                        || build_path(full_path, sizeof(full_path), path,
                                      list.items[actual].name) != 0) continue;
                    if (action == 1) {
                        if (view_file(full_path) != 0) {
                            snprintf(message, sizeof(message), "Read failed: %s",
                                     strerror(errno));
                        }
                    } else if (edit_file(full_path) != 0) {
                        snprintf(message, sizeof(message), "Edit failed: %s", strerror(errno));
                    }
                    if (load_entries(path, &list) != 0) {
                        snprintf(message, sizeof(message), "Refresh failed: %s",
                                 strerror(errno));
                    }
                    continue;
                }
            }
            if (list.items[actual].is_directory) {
                if (change_directory(path, sizeof(path), list.items[actual].name) != 0) {
                    snprintf(message, sizeof(message), "Cannot open directory: %s",
                             strerror(errno));
                } else {
                    selected = 0;
                    query[0] = '\0';
                    if (load_entries(path, &list) != 0) {
                        snprintf(message, sizeof(message), "Cannot read directory: %s",
                                 strerror(errno));
                    }
                }
            } else {
                char full_path[PATH_MAX];
                if (build_path(full_path, sizeof(full_path), path,
                               list.items[actual].name) == 0
                    && view_file(full_path) != 0) {
                    snprintf(message, sizeof(message), "Read failed: %s", strerror(errno));
                }
            }
        } else if (key == 'e') {
            actual = visible_index(&list, query, selected);
            if (actual < list.count && !list.items[actual].is_directory) {
                char full_path[PATH_MAX];
                if (build_path(full_path, sizeof(full_path), path,
                               list.items[actual].name) == 0
                    && edit_file(full_path) != 0) {
                    snprintf(message, sizeof(message), "Edit failed: %s", strerror(errno));
                }
                if (load_entries(path, &list) != 0) {
                    snprintf(message, sizeof(message), "Refresh failed: %s", strerror(errno));
                }
            }
        }
    }
    fputs("\033[<u", stdout);
    fflush(stdout);
    endwin();
    free_entries(&list);
    return 0;
}