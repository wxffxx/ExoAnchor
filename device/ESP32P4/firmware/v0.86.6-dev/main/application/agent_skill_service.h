#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "agent_router.h"

#define SI_AGENT_SKILL_CONFIGURE_UART_CLI "configure-uart-cli"
#define SI_AGENT_SKILL_CONFIGURE_ACCESS_VIA_KVM "configure-access-via-kvm"
#define SI_AGENT_SKILL_BRIDGE_UART_SSH_ACCESS "bridge-uart-ssh-access"

/*
 * Built-in skills are immutable, trusted firmware resources. The editable
 * skills_json setting is only an enable/disable overlay and never supplies
 * system-prompt text.
 */
size_t si_agent_skill_count(void);
const char *si_agent_skill_name_at(size_t index);
bool si_agent_skill_enabled(const char *skill_name);
bool si_agent_skill_should_activate(const char *skill_name,
                                    const si_agent_route_t *route,
                                    const char *message);
const char *si_agent_skill_content(const char *skill_name,
                                   size_t *content_length);
