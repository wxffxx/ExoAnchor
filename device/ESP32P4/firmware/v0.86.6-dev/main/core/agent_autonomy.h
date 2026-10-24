#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Embedded Agent autonomy, modelled on the SAE driving-automation levels.
 *
 * The level is an operator setting; the action class is derived by firmware
 * from the exact tool and arguments and is never taken from the model.
 *
 *            R0 read   R1 operate   R2 system   R3 irreversible
 *   L0       -         -            -           -        advice only
 *   L1       auto      -            -           -        (default)
 *   L2       auto      confirm      -           -
 *   L3       auto      auto         confirm     -
 *   L4       auto      auto         auto        confirm
 *   L5       not offered: irreversible actions always need a human.
 *
 * This module is pure and owns no device state.
 */

typedef enum {
    SI_AGENT_AUTONOMY_L0 = 0,
    SI_AGENT_AUTONOMY_L1 = 1,
    SI_AGENT_AUTONOMY_L2 = 2,
    SI_AGENT_AUTONOMY_L3 = 3,
    SI_AGENT_AUTONOMY_L4 = 4,
} si_agent_autonomy_level_t;

#define SI_AGENT_AUTONOMY_DEFAULT SI_AGENT_AUTONOMY_L1
#define SI_AGENT_AUTONOMY_MAX SI_AGENT_AUTONOMY_L4

typedef enum {
    SI_AGENT_ACTION_READ = 0,
    SI_AGENT_ACTION_OPERATE = 1,
    SI_AGENT_ACTION_SYSTEM = 2,
    SI_AGENT_ACTION_IRREVERSIBLE = 3,
} si_agent_action_class_t;

bool si_agent_autonomy_level_valid(int level);

/* Whether the level admits the class at all. */
bool si_agent_autonomy_allows(si_agent_autonomy_level_t level,
                              si_agent_action_class_t action_class);

/* Whether an admitted class runs without a per-action human decision. */
bool si_agent_autonomy_auto_approves(si_agent_autonomy_level_t level,
                                     si_agent_action_class_t action_class);

/* Highest class that is auto-approved, or -1 when nothing is. */
int si_agent_autonomy_auto_through(si_agent_autonomy_level_t level);

const char *si_agent_action_class_name(si_agent_action_class_t action_class);
const char *si_agent_autonomy_level_label(si_agent_autonomy_level_t level);

/*
 * Classify one shell command line.  Only commands that are provably
 * read-only (allowlisted programs, no shell control operators except pipes
 * between read-only programs, no credential paths) are R0.  Privilege
 * escalation, service/package/user/network management and writes to system
 * paths are R2; destructive storage, recursive deletion and power-off are
 * R3; everything else is R1.  NULL or empty input is R3.
 */
si_agent_action_class_t si_agent_classify_shell_command(const char *command);

/* Classify a write/create target path (file, directory, download). */
si_agent_action_class_t si_agent_classify_path_write(const char *path);

#ifdef __cplusplus
}
#endif
