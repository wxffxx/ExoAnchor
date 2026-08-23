// Transitional Agent module: Web Search tool. Shares the run context until its service API is finalized.
static bool agent_execute_web_search_tool(const char *job_id, const cJSON *call,
                                          int index, bool dry_run,
                                          bool allow_web_search,
                                          cJSON *results, bool *all_ok)
{
    const char *query = agent_tool_query(call);
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddNumberToObject(item, "index", index);
    cJSON_AddStringToObject(item, "tool", "web_search");
    cJSON_AddBoolToObject(item, "dry_run", dry_run);
    if (query) {
        cJSON_AddStringToObject(item, "query", query);
    }

    if (!query || !query[0] || strlen(query) > AGENT_WEB_SEARCH_QUERY_MAX_LEN) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "web_search query required or too large");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search invalid query",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    if (!allow_web_search) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "web_search not allowed for this Agent run");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad",
                             "tool[%d] web_search blocked · not allowed for this run",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return true;
    }

    agent_web_search_settings_t settings;
    agent_web_search_defaults(&settings);
    esp_err_t ret = agent_web_search_load(&settings);
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search settings failed · %s",
                             index + 1, esp_err_to_name(ret));
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddStringToObject(item, "profile", settings.profile);
    cJSON_AddStringToObject(item, "model", settings.model);
    cJSON_AddStringToObject(item, "strategy", settings.strategy);
    cJSON_AddBoolToObject(item, "enabled", settings.enabled);

    if (dry_run) {
        agent_run_add_eventf(job_id, "cmd", "tool[%d] web_search dry-run · %s",
                             index + 1, query);
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        cJSON_AddItemToArray(results, item);
        return false;
    }
    if (!settings.enabled) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "web_search disabled in Settings");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search disabled",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    agent_api_profile_t profile;
    ret = agent_api_get_profile(settings.profile, &profile);
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "web_search API profile invalid");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search profile invalid",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    const char *endpoint = settings.endpoint[0] ? settings.endpoint : profile.endpoint;
    if (!endpoint || !endpoint[0]) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "web_search endpoint not configured");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search endpoint missing",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddStringToObject(item, "endpoint_source",
                            settings.endpoint[0] ? "web_search" : "agent_profile");

    ret = agent_api_validate_endpoint(endpoint, false);
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "web_search endpoint must be non-empty HTTPS");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad",
                             "tool[%d] web_search endpoint invalid", index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    char api_key[AGENT_API_SECRET_MAX_LEN + 1] = {0};
    const char *secret_source = "none";
    ret = agent_web_search_get_secret(&settings, api_key, sizeof(api_key),
                                      &secret_source);
    if (ret != ESP_OK) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "web_search API key not configured");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search API key missing",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    cJSON_AddStringToObject(item, "api_key_source", secret_source);

    char url[AGENT_API_ENDPOINT_MAX_LEN + 24];
    ret = agent_build_chat_url(endpoint, url, sizeof(url));
    if (ret != ESP_OK) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "web_search endpoint URL is invalid");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad",
                             "tool[%d] web_search URL build failed", index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }
    char *body = agent_build_web_search_request(settings.model, query,
                                                settings.strategy);
    if (!body) {
        memset(api_key, 0, sizeof(api_key));
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "web_search request alloc failed");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search request alloc failed",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return false;
    }

    char *cloud_payload = NULL;
    int cloud_status = 0;
    char cloud_err[128] = {0};
    int64_t started_us = esp_timer_get_time();
    agent_run_add_eventf(job_id, "cmd", "tool[%d] web_search · %s",
                         index + 1, query);
    agent_run_add_eventf(job_id, "progress",
                         "tool[%d] web_search running · profile=%s model=%s",
                         index + 1, settings.profile, settings.model);
    ret = agent_cloud_post_json(job_id, url, api_key, body, false,
                                &cloud_payload, &cloud_status,
                                cloud_err, sizeof(cloud_err));
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    memset(api_key, 0, sizeof(api_key));
    free(body);
    cJSON_AddNumberToObject(item, "cloud_status", cloud_status);
    cJSON_AddNumberToObject(item, "elapsed_ms", elapsed_ms);
    if (ret != ESP_OK) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                cloud_err[0] ? cloud_err : esp_err_to_name(ret));
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search failed · %s",
                             index + 1,
                             cloud_err[0] ? cloud_err : esp_err_to_name(ret));
        free(cloud_payload);
        if (all_ok) {
            *all_ok = false;
        }
        return true;
    }
    if (cloud_status < 200 || cloud_status >= 300) {
        char msg[192];
        snprintf(msg, sizeof(msg), "web_search http status %d: %.120s",
                 cloud_status, cloud_payload ? cloud_payload : "");
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", msg);
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad",
                             "tool[%d] web_search http status %d · %lldms",
                             index + 1, cloud_status, (long long)elapsed_ms);
        free(cloud_payload);
        if (all_ok) {
            *all_ok = false;
        }
        return true;
    }

    cJSON *api_root = cJSON_Parse(cloud_payload);
    free(cloud_payload);
    if (!api_root) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "web_search returned invalid json");
        cJSON_AddItemToArray(results, item);
        agent_run_add_eventf(job_id, "bad", "tool[%d] web_search invalid json",
                             index + 1);
        if (all_ok) {
            *all_ok = false;
        }
        return true;
    }
    const char *content = agent_response_content(api_root);
    cJSON_AddBoolToObject(item, "ok", content && content[0]);
    if (content && content[0]) {
        agent_add_limited_tool_output(item, content);
        agent_run_add_eventf(job_id, "ok",
                             "tool[%d] web_search ok · http=%d · %lldms",
                             index + 1, cloud_status, (long long)elapsed_ms);
    } else {
        cJSON_AddStringToObject(item, "error", "web_search returned empty content");
        agent_run_add_eventf(job_id, "bad",
                             "tool[%d] web_search empty content · http=%d · %lldms",
                             index + 1, cloud_status, (long long)elapsed_ms);
        if (all_ok) {
            *all_ok = false;
        }
    }
    cJSON_Delete(api_root);
    cJSON_AddItemToArray(results, item);
    return true;
}
