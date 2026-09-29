// Bind the development UART diagnostics facade to the Web/Agent runtime.
static void register_diagnostics_backend(void)
{
    static const si_diagnostics_backend_t backend = {
        .web_client_count = si_web_client_count,
#if SI_CFG_EMBEDDED_AGENT_ENABLED
        .agent_history = web_debug_agent_history,
        .agent_history_append = web_debug_agent_history_append,
        .agent_history_clear = web_debug_agent_history_clear,
        .agent_memory = web_debug_agent_memory,
        .agent_memory_append = web_debug_agent_memory_append,
        .agent_memory_clear = web_debug_agent_memory_clear,
        .agent_data_clear = web_debug_agent_data_clear,
        .agent_run_start = web_debug_agent_run_start,
        .agent_run_status = web_debug_agent_run_status,
        .agent_run_events = web_debug_agent_run_events,
        .agent_run_pause = web_debug_agent_run_pause,
        .agent_run_resume = web_debug_agent_run_resume,
        .agent_run_cancel = web_debug_agent_run_cancel,
        .agent_run_abort = web_debug_agent_run_abort,
        .agent_run_steer = web_debug_agent_run_steer,
        .agent_run_result = web_debug_agent_run_result,
#endif
#if SI_CFG_EMBEDDED_AGENT_ENABLED
        .ssh_target = web_debug_ssh_target,
        .ssh_exec = web_debug_ssh_exec,
#endif
    };
    si_diagnostics_register_backend(&backend);
}
