#include "core/main.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void print_json_string(const char *text) {
    const unsigned char *p = (const unsigned char *)text;

    putchar('"');
    while (*p != '\0') {
        switch (*p) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20) {
                    printf("\\u%04x", (unsigned int)*p);
                } else {
                    putchar(*p);
                }
        }
        ++p;
    }
    putchar('"');
}

static int list_directory(const char *path, int gui_output) {
    DIR *directory = opendir(path);
    struct dirent *entry;
    int first_entry = 1;

    if (directory == NULL) {
        fprintf(stderr, "Cannot open directory '%s': %s\n", path, strerror(errno));
        return 1;
    }

    if (gui_output) {
        fputs("{\"path\":", stdout);
        print_json_string(path);
        fputs(",\"entries\":[", stdout);
    }

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (gui_output) {
            if (!first_entry) {
                putchar(',');
            }
            print_json_string(entry->d_name);
            first_entry = 0;
        } else {
            puts(entry->d_name);
        }
    }

    if (errno != 0) {
        fprintf(stderr, "Error reading directory '%s': %s\n", path, strerror(errno));
        closedir(directory);
        return 1;
    }

    if (closedir(directory) == -1) {
        fprintf(stderr, "Cannot close directory '%s': %s\n", path, strerror(errno));
        return 1;
    }

    if (gui_output) {
        fputs("]}\n", stdout);
    }
    return 0;
}

static int read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    unsigned char buffer[8192];
    size_t count;

    if (file == NULL) {
        fprintf(stderr, "Cannot read '%s': %s\n", path, strerror(errno));
        return 1;
    }

    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (fwrite(buffer, 1, count, stdout) != count) {
            fprintf(stderr, "Cannot write file contents to standard output: %s\n",
                    strerror(errno));
            fclose(file);
            return 1;
        }
    }

    if (ferror(file)) {
        fprintf(stderr, "Error reading '%s': %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }
    if (fclose(file) == EOF) {
        fprintf(stderr, "Cannot close '%s': %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static int write_file(const char *path) {
    FILE *file = fopen(path, "wb");
    unsigned char buffer[8192];
    size_t count;

    if (file == NULL) {
        fprintf(stderr, "Cannot write '%s': %s\n", path, strerror(errno));
        return 1;
    }

    while ((count = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        if (fwrite(buffer, 1, count, file) != count) {
            fprintf(stderr, "Error writing '%s': %s\n", path, strerror(errno));
            fclose(file);
            return 1;
        }
    }

    if (ferror(stdin)) {
        fprintf(stderr, "Error reading standard input: %s\n", strerror(errno));
        fclose(file);
        return 1;
    }
    if (fclose(file) == EOF) {
        fprintf(stderr, "Cannot close '%s': %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

static void print_usage(const char *program) {
    printf("Usage:\n"
           "  %s [directory]                 List a directory\n"
           "  %s --gui [directory]           Print a JSON directory listing\n"
           "  %s --tui [directory]           Open the interactive file manager\n"
           "  %s --windows [directory]       Open the two-pane TUI file manager\n"
           "  %s --read FILE                 Print a file to standard output\n"
           "  %s --write FILE                Write standard input to a file\n"
           "\n"
           "TUI keys: arrows move, Enter opens, / searches, e edits, n creates,\n"
           "          Shift+Enter opens the context menu, : runs a shell command,\n"
           "          Backspace goes to the parent directory, q quits.\n",
           program, program, program, program, program, program);
}

int main(int argc, char *argv[]) {
    const char *path = ".";
    const char *file_path = NULL;
    int mode = 0;
    int path_provided = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--gui") == 0) {
            if (mode != 0) goto invalid_arguments;
            mode = 1;
        } else if (strcmp(argv[i], "--tui") == 0 || strcmp(argv[i], "--windows") == 0) {
            if (mode != 0) goto invalid_arguments;
            mode = 2;
        } else if (strcmp(argv[i], "--read") == 0 || strcmp(argv[i], "-read") == 0) {
            if (mode != 0 || i + 1 >= argc) goto invalid_arguments;
            mode = 3;
            file_path = argv[++i];
        } else if (strcmp(argv[i], "--write") == 0 || strcmp(argv[i], "-write") == 0) {
            if (mode != 0 || i + 1 >= argc) goto invalid_arguments;
            mode = 4;
            file_path = argv[++i];
        } else if (argv[i][0] == '-' || path_provided) {
            goto invalid_arguments;
        } else {
            path = argv[i];
            path_provided = 1;
        }
    }

    if (mode == 3) {
        if (path_provided) goto invalid_arguments;
        return read_file(file_path);
    }
    if (mode == 4) {
        if (path_provided) goto invalid_arguments;
        return write_file(file_path);
    }
    if (mode == 2) {
        return run_tui(path);
    }
    return list_directory(path, mode == 1);

invalid_arguments:
    print_usage(argv[0]);
    return 2;
}
