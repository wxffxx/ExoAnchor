// Transitional domain module: Storage HTTP. Kept in the web compilation unit until its service contract is independent.
static bool storage_parent_dir_exists(const char *abs)
{
    if (!abs || !abs[0]) {
        return false;
    }
    char parent[STORAGE_LIST_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
    strlcpy(parent, abs, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) {
        return false;
    }
    *slash = '\0';
    struct stat st;
    return stat(parent, &st) == 0 && S_ISDIR(st.st_mode);
}

static esp_err_t storage_send_file(httpd_req_t *req, const char *abs,
                                   const char *rel, bool attachment)
{
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) {
        return si_http_send_text_status(req, "404 Not Found", "storage file not found");
    }

    FILE *file = fopen(abs, "rb");
    if (!file) {
        ESP_LOGW(TAG, "Failed to open storage file %s: errno=%d", abs, errno);
        return si_http_send_text_status(req, "500 Internal Server Error", "storage file open failed");
    }

    httpd_resp_set_type(req, storage_mime_type(rel));
    if (attachment) {
        char disposition[STORAGE_LIST_PATH_MAX + 48] = {0};
        snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"",
                 storage_basename(rel));
        httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buf = malloc(STORAGE_IO_BUFFER_SIZE);
    if (!buf) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    while (!feof(file)) {
        size_t n = fread(buf, 1, STORAGE_IO_BUFFER_SIZE, file);
        if (n > 0 && httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
        if (ferror(file)) {
            ESP_LOGW(TAG, "Failed to read storage file %s: errno=%d", abs, errno);
            ret = ESP_FAIL;
            break;
        }
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    free(buf);
    fclose(file);
    return ret;
}

static bool storage_preview_is_text(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return true;
    }
    size_t suspicious = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = data[i];
        if (ch == 0) {
            return false;
        }
        if (ch < 0x09 || (ch > 0x0D && ch < 0x20)) {
            suspicious++;
        }
    }
    return suspicious * 12U < len;
}

static char *storage_hex_preview(const uint8_t *data, size_t len)
{
    size_t cap = (len / 16U + 1U) * 84U + 1U;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }
    out[0] = '\0';
    char line[96] = {0};
    for (size_t off = 0; off < len; off += 16U) {
        size_t pos = 0;
        int n = snprintf(line, sizeof(line), "%04x  ", (unsigned)off);
        if (n < 0) {
            continue;
        }
        pos = (size_t)n;
        for (size_t i = 0; i < 16U; i++) {
            if (off + i < len) {
                n = snprintf(line + pos, sizeof(line) - pos, "%02x ", data[off + i]);
            } else {
                n = snprintf(line + pos, sizeof(line) - pos, "   ");
            }
            if (n < 0) {
                break;
            }
            pos += (size_t)n;
        }
        n = snprintf(line + pos, sizeof(line) - pos, " |");
        if (n < 0) {
            continue;
        }
        pos += (size_t)n;
        for (size_t i = 0; i < 16U && off + i < len; i++) {
            uint8_t ch = data[off + i];
            line[pos++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
            if (pos >= sizeof(line) - 3U) {
                break;
            }
        }
        line[pos++] = '|';
        line[pos++] = '\n';
        line[pos] = '\0';
        strlcat(out, line, cap);
    }
    return out;
}

static esp_err_t storage_list_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_STORAGE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    si_storage_tf_status_t tf_info;
    si_storage_get_tf_status(&tf_info);
    if (!tf_info.mounted) {
        return si_http_send_text_status(req, "409 Conflict",
                                tf_info.last_error[0] ? tf_info.last_error : "TF card not mounted");
    }

    char query[STORAGE_LIST_QUERY_MAX] = {0};
    char requested[STORAGE_LIST_PATH_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "path", requested, sizeof(requested));
    }
    if (si_storage_url_decode_inplace(requested) != SI_STORAGE_PATH_OK) {
        return si_http_send_text_status(req, "400 Bad Request", "invalid storage path encoding");
    }

    char rel[STORAGE_LIST_PATH_MAX] = {0};
    char abs[STORAGE_LIST_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
    if (si_storage_resolve_path(requested, SI_STORAGE_ROOT,
                                rel, sizeof(rel), abs, sizeof(abs)) != SI_STORAGE_PATH_OK) {
        return si_http_send_text_status(req, "400 Bad Request", "invalid storage path");
    }
    esp_err_t ret = ESP_OK;

    struct stat base_st;
    if (stat(abs, &base_st) != 0 || !S_ISDIR(base_st.st_mode)) {
        return si_http_send_text_status(req, "404 Not Found", "storage directory not found");
    }

    DIR *dir = opendir(abs);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open storage directory %s: errno=%d", abs, errno);
        return si_http_send_text_status(req, "500 Internal Server Error", "storage directory open failed");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "root", SI_STORAGE_ROOT);
    cJSON_AddStringToObject(root, "path", rel);
    char display_path[STORAGE_LIST_PATH_MAX + 8] = {0};
    snprintf(display_path, sizeof(display_path), "/EA%s%s", rel[0] ? "/" : "", rel);
    cJSON_AddStringToObject(root, "display_path", display_path);
    cJSON_AddNumberToObject(root, "max_items", STORAGE_LIST_MAX_ITEMS);

    char parent[STORAGE_LIST_PATH_MAX] = {0};
    if (rel[0]) {
        strlcpy(parent, rel, sizeof(parent));
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
        } else {
            parent[0] = '\0';
        }
    }
    cJSON_AddStringToObject(root, "parent", parent);

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    uint32_t count = 0;
    bool truncated = false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (count >= STORAGE_LIST_MAX_ITEMS) {
            truncated = true;
            break;
        }

        char child_rel[STORAGE_LIST_PATH_MAX] = {0};
        int rel_written = rel[0] ?
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, ent->d_name) :
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        if (rel_written < 0 || (size_t)rel_written >= sizeof(child_rel)) {
            truncated = true;
            continue;
        }

        char child_abs[STORAGE_LIST_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
        int abs_written = snprintf(child_abs, sizeof(child_abs), "%s/%s",
                                   SI_STORAGE_ROOT, child_rel);
        if (abs_written < 0 || (size_t)abs_written >= sizeof(child_abs)) {
            truncated = true;
            continue;
        }

        struct stat st;
        bool have_stat = stat(child_abs, &st) == 0;
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            truncated = true;
            break;
        }
        cJSON_AddStringToObject(item, "name", ent->d_name);
        cJSON_AddStringToObject(item, "path", child_rel);
        cJSON_AddStringToObject(item, "type", have_stat ? storage_file_type(&st) : "unknown");
        cJSON_AddNumberToObject(item, "size", have_stat && S_ISREG(st.st_mode) ? (double)st.st_size : 0);
        cJSON_AddNumberToObject(item, "mtime", have_stat ? (double)st.st_mtime : 0);
        cJSON_AddItemToArray(entries, item);
        count++;
    }
    closedir(dir);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddBoolToObject(root, "truncated", truncated);

    ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t storage_upload_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_STORAGE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    si_storage_tf_status_t tf_info;
    si_storage_get_tf_status(&tf_info);
    if (!tf_info.mounted) {
        return si_http_send_text_status(req, "409 Conflict",
                                tf_info.last_error[0] ? tf_info.last_error : "TF card not mounted");
    }
    if (req->content_len <= 0) {
        return si_http_send_text_status(req, "400 Bad Request", "upload body is empty");
    }
    if (req->content_len > STORAGE_UPLOAD_MAX_BYTES) {
        return si_http_send_text_status(req, "413 Payload Too Large", "uploaded file is too large");
    }

    char query[STORAGE_LIST_QUERY_MAX] = {0};
    char requested[STORAGE_LIST_PATH_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", requested, sizeof(requested)) != ESP_OK ||
        !requested[0]) {
        return si_http_send_text_status(req, "400 Bad Request", "storage upload path required");
    }
    if (si_storage_url_decode_inplace(requested) != SI_STORAGE_PATH_OK) {
        return si_http_send_text_status(req, "400 Bad Request", "invalid storage path encoding");
    }

    char rel[STORAGE_LIST_PATH_MAX] = {0};
    char abs[STORAGE_LIST_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
    if (si_storage_resolve_path(requested, SI_STORAGE_ROOT,
                                rel, sizeof(rel), abs, sizeof(abs)) != SI_STORAGE_PATH_OK ||
        !si_storage_path_is_file_target(rel)) {
        return si_http_send_text_status(req, "400 Bad Request", "storage upload path is not allowed");
    }
    esp_err_t ret = ESP_OK;
    ret = si_storage_ensure_layout();
    if (ret != ESP_OK) {
        return si_http_send_text_status(req, "500 Internal Server Error", "storage layout unavailable");
    }
    if (!storage_parent_dir_exists(abs)) {
        return si_http_send_text_status(req, "404 Not Found", "storage upload directory not found");
    }

    char temp_path[sizeof(abs) + 32];
    char backup_path[sizeof(abs) + 32];
    uint32_t upload_id = esp_random();
    snprintf(temp_path, sizeof(temp_path), "%s.upload-%08" PRIx32,
             abs, upload_id);
    snprintf(backup_path, sizeof(backup_path), "%s.backup-%08" PRIx32,
             abs, upload_id);

    FILE *file = fopen(temp_path, "wb");
    if (!file) {
        ESP_LOGW(TAG, "Failed to open storage upload temp file: errno=%d",
                 errno);
        return si_http_send_text_status(req, "500 Internal Server Error", "storage upload open failed");
    }

    char *buf = malloc(STORAGE_IO_BUFFER_SIZE);
    if (!buf) {
        fclose(file);
        (void)remove(temp_path);
        return ESP_ERR_NO_MEM;
    }

    size_t remaining = req->content_len;
    size_t written = 0;
    while (remaining > 0) {
        size_t to_read = MIN(remaining, (size_t)STORAGE_IO_BUFFER_SIZE);
        int read_len = httpd_req_recv(req, buf, to_read);
        if (read_len <= 0) {
            ret = ESP_FAIL;
            break;
        }
        if (fwrite(buf, 1, (size_t)read_len, file) != (size_t)read_len) {
            ret = ESP_FAIL;
            break;
        }
        remaining -= (size_t)read_len;
        written += (size_t)read_len;
    }

    free(buf);
    if (ret == ESP_OK && fflush(file) != 0) {
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK && fsync(fileno(file)) != 0) {
        ret = ESP_FAIL;
    }
    if (fclose(file) != 0) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK) {
        (void)remove(temp_path);
        ESP_LOGW(TAG, "Storage upload failed for %s", abs);
        return si_http_send_text_status(req, "500 Internal Server Error", "storage upload failed");
    }

    bool had_original = access(abs, F_OK) == 0;
    if (had_original && rename(abs, backup_path) != 0) {
        (void)remove(temp_path);
        return si_http_send_text_status(req, "500 Internal Server Error",
                                        "storage upload backup failed");
    }
    if (rename(temp_path, abs) != 0) {
        if (had_original) {
            (void)rename(backup_path, abs);
        }
        (void)remove(temp_path);
        return si_http_send_text_status(req, "500 Internal Server Error",
                                        "storage upload commit failed");
    }
    if (had_original) {
        (void)remove(backup_path);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "path", rel);
    char display_path[STORAGE_LIST_PATH_MAX + 8] = {0};
    snprintf(display_path, sizeof(display_path), "/EA/%s", rel);
    cJSON_AddStringToObject(root, "display_path", display_path);
    cJSON_AddNumberToObject(root, "size", (double)written);
    si_web_log("INFO", "TF file uploaded");
    ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t storage_download_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_STORAGE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    si_storage_tf_status_t tf_info;
    si_storage_get_tf_status(&tf_info);
    if (!tf_info.mounted) {
        return si_http_send_text_status(req, "409 Conflict",
                                tf_info.last_error[0] ? tf_info.last_error : "TF card not mounted");
    }

    char query[STORAGE_LIST_QUERY_MAX] = {0};
    char requested[STORAGE_LIST_PATH_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", requested, sizeof(requested)) != ESP_OK ||
        !requested[0]) {
        return si_http_send_text_status(req, "400 Bad Request", "storage download path required");
    }
    if (si_storage_url_decode_inplace(requested) != SI_STORAGE_PATH_OK) {
        return si_http_send_text_status(req, "400 Bad Request", "invalid storage path encoding");
    }

    char rel[STORAGE_LIST_PATH_MAX] = {0};
    char abs[STORAGE_LIST_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
    if (si_storage_resolve_path(requested, SI_STORAGE_ROOT,
                                rel, sizeof(rel), abs, sizeof(abs)) != SI_STORAGE_PATH_OK ||
        !si_storage_path_is_file_target(rel)) {
        return si_http_send_text_status(req, "400 Bad Request", "storage download path is not allowed");
    }
    return storage_send_file(req, abs, rel, true);
}

static esp_err_t storage_view_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_STORAGE, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }

    si_storage_tf_status_t tf_info;
    si_storage_get_tf_status(&tf_info);
    if (!tf_info.mounted) {
        return si_http_send_text_status(req, "409 Conflict",
                                tf_info.last_error[0] ? tf_info.last_error : "TF card not mounted");
    }

    char query[STORAGE_LIST_QUERY_MAX] = {0};
    char requested[STORAGE_LIST_PATH_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", requested, sizeof(requested)) != ESP_OK ||
        !requested[0]) {
        return si_http_send_text_status(req, "400 Bad Request", "storage view path required");
    }
    if (si_storage_url_decode_inplace(requested) != SI_STORAGE_PATH_OK) {
        return si_http_send_text_status(req, "400 Bad Request", "invalid storage path encoding");
    }

    char rel[STORAGE_LIST_PATH_MAX] = {0};
    char abs[STORAGE_LIST_PATH_MAX + sizeof(SI_STORAGE_ROOT) + 4] = {0};
    if (si_storage_resolve_path(requested, SI_STORAGE_ROOT,
                                rel, sizeof(rel), abs, sizeof(abs)) != SI_STORAGE_PATH_OK ||
        !si_storage_path_is_file_target(rel)) {
        return si_http_send_text_status(req, "400 Bad Request", "storage view path is not allowed");
    }

    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) {
        return si_http_send_text_status(req, "404 Not Found", "storage file not found");
    }

    FILE *file = fopen(abs, "rb");
    if (!file) {
        return si_http_send_text_status(req, "500 Internal Server Error", "storage file open failed");
    }

    size_t limit = MIN((size_t)st.st_size, (size_t)STORAGE_VIEW_MAX_BYTES);
    uint8_t *buf = calloc(1, limit + 1U);
    if (!buf) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t nread = fread(buf, 1, limit, file);
    fclose(file);

    bool text = storage_preview_is_text(buf, nread);
    char *content = NULL;
    if (text) {
        content = malloc(nread + 1U);
        if (content) {
            memcpy(content, buf, nread);
            content[nread] = '\0';
        }
    } else {
        content = storage_hex_preview(buf, nread);
    }
    free(buf);
    if (!content) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "path", rel);
    cJSON_AddStringToObject(root, "name", storage_basename(rel));
    cJSON_AddStringToObject(root, "mime", storage_mime_type(rel));
    cJSON_AddStringToObject(root, "mode", text ? "text" : "hex");
    cJSON_AddNumberToObject(root, "size", (double)st.st_size);
    cJSON_AddNumberToObject(root, "preview_bytes", (double)nread);
    cJSON_AddBoolToObject(root, "truncated", (size_t)st.st_size > nread);
    cJSON_AddStringToObject(root, "content", content);
    free(content);
    esp_err_t ret = si_http_send_json(req, root);
    cJSON_Delete(root);
    return ret;
}
