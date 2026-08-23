#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SI_STORAGE_PATH_OK = 0,
    SI_STORAGE_PATH_INVALID_ARGUMENT,
    SI_STORAGE_PATH_TOO_LONG,
} si_storage_path_result_t;

si_storage_path_result_t si_storage_url_decode_inplace(char *value);

si_storage_path_result_t si_storage_resolve_path(const char *input,
                                                 const char *storage_root,
                                                 char *relative_path,
                                                 size_t relative_path_size,
                                                 char *absolute_path,
                                                 size_t absolute_path_size);

bool si_storage_path_is_file_target(const char *relative_path);
