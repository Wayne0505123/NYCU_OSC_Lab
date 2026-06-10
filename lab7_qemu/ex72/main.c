#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH      4096
#define MAX_COMPONENT 256

const char* curr_working_dir = "/path/to/current/directory";

/**
 * Resolve a relative or absolute filepath
 */
char* my_realpath(const char* path, char* resolved_path) {
    if (path == NULL || resolved_path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (path[0] == '/') {
        strcpy(resolved_path, "/");
    } else {
        if (strlen(curr_working_dir) >= MAX_PATH) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        strcpy(resolved_path, curr_working_dir);
    }

    while (*path != '\0') {
        while (*path == '/')
            path++;

        const char* component = path;
        while (*path != '\0' && *path != '/')
            path++;

        size_t len = path - component;
        if (len == 0 || (len == 1 && component[0] == '.'))
            continue;

        if (len == 2 && component[0] == '.' && component[1] == '.') {
            if (strcmp(resolved_path, "/") != 0) {
                char* last_slash = strrchr(resolved_path, '/');
                if (last_slash == resolved_path)
                    resolved_path[1] = '\0';
                else if (last_slash != NULL)
                    *last_slash = '\0';
            }
            continue;
        }

        if (len >= MAX_COMPONENT) {
            errno = ENAMETOOLONG;
            return NULL;
        }

        size_t resolved_len = strlen(resolved_path);
        size_t need_slash = resolved_len > 1 ? 1 : 0;
        if (resolved_len + need_slash + len >= MAX_PATH) {
            errno = ENAMETOOLONG;
            return NULL;
        }

        if (need_slash)
            resolved_path[resolved_len++] = '/';
        memcpy(resolved_path + resolved_len, component, len);
        resolved_path[resolved_len + len] = '\0';
    }

    return resolved_path;
}

int main() {
    char resolved[MAX_PATH];
    const char* test_paths[] = {
        ".",
        "..",
        "./test",
        "../parent",
        "dir1/dir2/../../dir3",
        "/absolute/path",
        "relative/./path",
        NULL,
    };
    for (int i = 0; test_paths[i] != NULL; i++) {
        printf("[%d] \"%s\"", i, test_paths[i]);
        if (my_realpath(test_paths[i], resolved))
            printf(" --> \"%s\"\n", resolved);
    }
    return 0;
}
