#include "agent_http_routes.h"

#include <stddef.h>

#include "esp_check.h"

static const char *TAG = "si-agent-http";

typedef struct {
    const char *uri;
    httpd_method_t method;
    si_agent_http_handler_t handler;
} agent_http_route_t;

static esp_err_t register_route(httpd_handle_t server,
                                const agent_http_route_t *route)
{
    ESP_RETURN_ON_FALSE(server && route && route->uri && route->handler,
                        ESP_ERR_INVALID_ARG, TAG, "invalid Agent HTTP route");
    const httpd_uri_t config = {
        .uri = route->uri,
        .method = route->method,
        .handler = route->handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &config);
}

esp_err_t si_agent_http_routes_register(httpd_handle_t server,
                                        const si_agent_http_handlers_t *handlers)
{
    ESP_RETURN_ON_FALSE(server && handlers, ESP_ERR_INVALID_ARG, TAG,
                        "Agent HTTP handlers are required");

    const agent_http_route_t routes[] = {
        {.uri = "/api/settings/agent-api", .method = HTTP_GET,
         .handler = handlers->settings_api},
        {.uri = "/api/settings/agent-api", .method = HTTP_POST,
         .handler = handlers->settings_api},
        {.uri = "/api/settings/agent-api/models", .method = HTTP_GET,
         .handler = handlers->settings_models},
        {.uri = "/api/settings/agent-prompt", .method = HTTP_GET,
         .handler = handlers->settings_prompt},
        {.uri = "/api/settings/agent-prompt", .method = HTTP_POST,
         .handler = handlers->settings_prompt},
        {.uri = "/api/settings/agent-tools", .method = HTTP_GET,
         .handler = handlers->settings_tools},
        {.uri = "/api/settings/agent-tools", .method = HTTP_POST,
         .handler = handlers->settings_tools},
        {.uri = "/api/agent/sessions", .method = HTTP_GET,
         .handler = handlers->sessions_get},
        {.uri = "/api/agent/sessions", .method = HTTP_POST,
         .handler = handlers->sessions_post},
        {.uri = "/api/agent/sessions/delete", .method = HTTP_POST,
         .handler = handlers->sessions_delete},
        {.uri = "/api/agent/history", .method = HTTP_GET,
         .handler = handlers->history_get},
        {.uri = "/api/agent/history", .method = HTTP_POST,
         .handler = handlers->history_post},
        {.uri = "/api/agent/history/clear", .method = HTTP_POST,
         .handler = handlers->history_clear},
        {.uri = "/api/agent/memory", .method = HTTP_GET,
         .handler = handlers->memory_get},
        {.uri = "/api/agent/memory", .method = HTTP_POST,
         .handler = handlers->memory_post},
        {.uri = "/api/agent/memory/clear", .method = HTTP_POST,
         .handler = handlers->memory_clear},
        {.uri = "/api/agent/data/clear", .method = HTTP_POST,
         .handler = handlers->data_clear},
        {.uri = "/api/agent/run", .method = HTTP_POST,
         .handler = handlers->run_submit},
        {.uri = "/api/agent/run/status", .method = HTTP_GET,
         .handler = handlers->run_status},
        {.uri = "/api/agent/run/events", .method = HTTP_GET,
         .handler = handlers->run_events},
        {.uri = "/api/agent/run/pause", .method = HTTP_POST,
         .handler = handlers->run_pause},
        {.uri = "/api/agent/run/resume", .method = HTTP_POST,
         .handler = handlers->run_resume},
        {.uri = "/api/agent/run/abort", .method = HTTP_POST,
         .handler = handlers->run_abort},
        {.uri = "/api/agent/run/cancel", .method = HTTP_POST,
         .handler = handlers->run_cancel},
        {.uri = "/api/agent/run/steer", .method = HTTP_POST,
         .handler = handlers->run_steer},
        {.uri = "/api/agent/requests", .method = HTTP_GET,
         .handler = handlers->requests_get},
        {.uri = "/api/agent/requests/decision", .method = HTTP_POST,
         .handler = handlers->requests_decision},
        {.uri = "/api/agent/requests/cancel", .method = HTTP_POST,
         .handler = handlers->requests_cancel},
        {.uri = "/api/agent/context-catalog", .method = HTTP_GET,
         .handler = handlers->context_catalog},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(register_route(server, &routes[i]), TAG,
                            "register Agent HTTP route %s", routes[i].uri);
    }
    return ESP_OK;
}
