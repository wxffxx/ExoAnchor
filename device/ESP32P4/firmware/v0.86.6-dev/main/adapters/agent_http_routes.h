#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// HTTP handler contract for the Agent settings and runtime API surface.
// Handler implementations may move between services without changing routes.
typedef esp_err_t (*si_agent_http_handler_t)(httpd_req_t *req);

typedef struct {
    si_agent_http_handler_t settings_api;
    si_agent_http_handler_t settings_models;
    si_agent_http_handler_t settings_prompt;
    si_agent_http_handler_t settings_tools;
    si_agent_http_handler_t sessions_get;
    si_agent_http_handler_t sessions_post;
    si_agent_http_handler_t sessions_delete;
    si_agent_http_handler_t history_get;
    si_agent_http_handler_t history_post;
    si_agent_http_handler_t history_clear;
    si_agent_http_handler_t memory_get;
    si_agent_http_handler_t memory_post;
    si_agent_http_handler_t memory_clear;
    si_agent_http_handler_t data_clear;
    si_agent_http_handler_t run_submit;
    si_agent_http_handler_t run_status;
    si_agent_http_handler_t run_events;
    si_agent_http_handler_t run_pause;
    si_agent_http_handler_t run_resume;
    si_agent_http_handler_t run_abort;
    si_agent_http_handler_t run_cancel;
    si_agent_http_handler_t run_steer;
    si_agent_http_handler_t requests_get;
    si_agent_http_handler_t requests_decision;
    si_agent_http_handler_t requests_cancel;
    si_agent_http_handler_t context_catalog;
} si_agent_http_handlers_t;

esp_err_t si_agent_http_routes_register(httpd_handle_t server,
                                        const si_agent_http_handlers_t *handlers);
