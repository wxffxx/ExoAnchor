#include "agent_autonomy.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define CLASSIFY_MAX_COMMAND 1024U
#define CLASSIFY_MAX_TOKENS 96U
#define CLASSIFY_MAX_TOKEN 128U

typedef struct {
    char text[CLASSIFY_MAX_TOKEN];
    unsigned segment;
    bool after_redirect;
} classify_token_t;

typedef struct {
    classify_token_t tokens[CLASSIFY_MAX_TOKENS];
    size_t count;
    unsigned segments;
    bool control;       /* ; & ` $ ( ) newline, ||, && */
    bool redirect;      /* > or < */
    bool truncated;
} classify_command_t;

bool si_agent_autonomy_level_valid(int level)
{
    return level >= (int)SI_AGENT_AUTONOMY_L0 &&
           level <= (int)SI_AGENT_AUTONOMY_MAX;
}

bool si_agent_autonomy_allows(si_agent_autonomy_level_t level,
                              si_agent_action_class_t action_class)
{
    if (!si_agent_autonomy_level_valid((int)level) ||
        action_class > SI_AGENT_ACTION_IRREVERSIBLE) {
        return false;
    }
    return level != SI_AGENT_AUTONOMY_L0 &&
           (int)action_class <= (int)level - 1;
}

int si_agent_autonomy_auto_through(si_agent_autonomy_level_t level)
{
    if (!si_agent_autonomy_level_valid((int)level) ||
        level == SI_AGENT_AUTONOMY_L0) {
        return -1;
    }
    if (level == SI_AGENT_AUTONOMY_L1) {
        return (int)SI_AGENT_ACTION_READ;
    }
    /* Irreversible actions are never auto-approved at any level. */
    int through = (int)level - 2;
    return through >= (int)SI_AGENT_ACTION_IRREVERSIBLE ?
           (int)SI_AGENT_ACTION_SYSTEM : through;
}

bool si_agent_autonomy_auto_approves(si_agent_autonomy_level_t level,
                                     si_agent_action_class_t action_class)
{
    return si_agent_autonomy_allows(level, action_class) &&
           (int)action_class <= si_agent_autonomy_auto_through(level);
}

const char *si_agent_action_class_name(si_agent_action_class_t action_class)
{
    switch (action_class) {
    case SI_AGENT_ACTION_READ: return "read";
    case SI_AGENT_ACTION_OPERATE: return "operate";
    case SI_AGENT_ACTION_SYSTEM: return "system";
    case SI_AGENT_ACTION_IRREVERSIBLE: return "irreversible";
    default: return "irreversible";
    }
}

const char *si_agent_autonomy_level_label(si_agent_autonomy_level_t level)
{
    switch (level) {
    case SI_AGENT_AUTONOMY_L0: return "L0 无自动化";
    case SI_AGENT_AUTONOMY_L1: return "L1 辅助";
    case SI_AGENT_AUTONOMY_L2: return "L2 部分自动化";
    case SI_AGENT_AUTONOMY_L3: return "L3 有条件自动化";
    case SI_AGENT_AUTONOMY_L4: return "L4 高度自动化";
    default: return "unknown";
    }
}

static bool word_char(char ch)
{
    return ch && !isspace((unsigned char)ch) &&
           !strchr("|;&<>()`$\"'", ch);
}

static void classify_parse(const char *command, classify_command_t *out)
{
    memset(out, 0, sizeof(*out));
    bool after_redirect = false;
    for (const char *p = command; *p;) {
        char ch = *p;
        if (isspace((unsigned char)ch)) {
            if (ch == '\n' || ch == '\r') {
                out->control = true;
                out->segments++;
            }
            p++;
            continue;
        }
        if (ch == '|') {
            if (p[1] == '|') {
                out->control = true;
                p++;
            }
            out->segments++;
            after_redirect = false;
            p++;
            continue;
        }
        if (strchr(";&`$()", ch)) {
            out->control = true;
            out->segments++;
            after_redirect = false;
            p++;
            continue;
        }
        if (ch == '>' || ch == '<') {
            out->redirect = true;
            after_redirect = ch == '>';
            p++;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            p++;
            continue;
        }
        size_t len = 0;
        classify_token_t *token = out->count < CLASSIFY_MAX_TOKENS ?
                                  &out->tokens[out->count] : NULL;
        while (word_char(*p)) {
            if (token && len + 1U < CLASSIFY_MAX_TOKEN) {
                token->text[len++] = (char)tolower((unsigned char)*p);
            } else {
                out->truncated = true;
            }
            p++;
        }
        if (token) {
            token->text[len] = '\0';
            token->segment = out->segments;
            token->after_redirect = after_redirect;
            out->count++;
        } else {
            out->truncated = true;
        }
        after_redirect = false;
    }
}

static bool in_list(const char *word, const char *const *list)
{
    for (size_t i = 0; list[i]; i++) {
        if (strcmp(word, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool starts_with(const char *value, const char *prefix)
{
    return strncmp(value, prefix, strlen(prefix)) == 0;
}

static const char *program_name(const char *token)
{
    const char *slash = strrchr(token, '/');
    return slash ? slash + 1 : token;
}

static bool segment_has(const classify_command_t *cmd, size_t first,
                        const char *const *words)
{
    unsigned segment = cmd->tokens[first].segment;
    for (size_t i = first + 1; i < cmd->count &&
         cmd->tokens[i].segment == segment; i++) {
        if (in_list(cmd->tokens[i].text, words)) {
            return true;
        }
    }
    return false;
}

static bool segment_has_prefix(const classify_command_t *cmd, size_t first,
                               const char *prefix)
{
    unsigned segment = cmd->tokens[first].segment;
    for (size_t i = first + 1; i < cmd->count &&
         cmd->tokens[i].segment == segment; i++) {
        if (starts_with(cmd->tokens[i].text, prefix)) {
            return true;
        }
    }
    return false;
}

static bool recursive_rm(const classify_command_t *cmd, size_t first)
{
    unsigned segment = cmd->tokens[first].segment;
    for (size_t i = first + 1; i < cmd->count &&
         cmd->tokens[i].segment == segment; i++) {
        const char *arg = cmd->tokens[i].text;
        if (strcmp(arg, "--recursive") == 0 ||
            (arg[0] == '-' && arg[1] != '-' &&
             (strchr(arg, 'r') || strchr(arg, 'R')))) {
            return true;
        }
    }
    return false;
}

static const char *const SYSTEM_PREFIXES[] = {
    "/etc", "/usr", "/boot", "/lib", "/lib64", "/bin", "/sbin", "/opt",
    "/var/lib", "/var/spool", "/var/log", "/sys", "/proc", "/root", "/srv",
    "/efi", NULL,
};

static bool system_path(const char *path)
{
    if (strcmp(path, "/") == 0) {
        return true;
    }
    for (size_t i = 0; SYSTEM_PREFIXES[i]; i++) {
        size_t len = strlen(SYSTEM_PREFIXES[i]);
        if (strncmp(path, SYSTEM_PREFIXES[i], len) == 0 &&
            (path[len] == '\0' || path[len] == '/')) {
            return true;
        }
    }
    return false;
}

static bool sensitive_token(const char *token)
{
    static const char *const markers[] = {
        "shadow", "gshadow", "/.ssh/", "id_rsa", "id_ed25519", "id_ecdsa",
        "id_dsa", ".pem", ".p12", ".pfx", ".netrc", ".pgpass",
        ".git-credentials", "credentials", "secret", "token", ".env",
        "private", "sudoers", ".kube/config", ".docker/config", NULL,
    };
    for (size_t i = 0; markers[i]; i++) {
        if (strstr(token, markers[i])) {
            return true;
        }
    }
    size_t len = strlen(token);
    return len > 4U && strcmp(token + len - 4U, ".key") == 0;
}

static si_agent_action_class_t classify_irreversible_or_system(
    const char *raw, const classify_command_t *cmd)
{
    static const char *const irreversible[] = {
        "wipefs", "fdisk", "sfdisk", "cfdisk", "parted", "sgdisk", "gdisk",
        "shred", "poweroff", "shutdown", "halt", "blkdiscard", "mkswap",
        "cryptsetup", "zpool", "zfs", "lvremove", "vgremove", "pvremove",
        "dd", "telinit", "nvme", "hdparm", "flashrom", NULL,
    };
    static const char *const system[] = {
        "sudo", "su", "doas", "pkexec", "runuser", "chroot", "apt",
        "apt-get", "aptitude", "dnf", "yum", "pacman", "zypper", "apk",
        "snap", "flatpak", "useradd", "userdel", "usermod", "groupadd",
        "groupdel", "gpasswd", "passwd", "chpasswd", "chage", "visudo",
        "mount", "umount", "swapon", "swapoff", "modprobe", "insmod",
        "rmmod", "iptables", "ip6tables", "nft", "ufw", "firewall-cmd",
        "nmcli", "reboot", "kill", "killall", "pkill", "service",
        "update-grub", "grub-install", "grub-mkconfig", "efibootmgr",
        "setenforce", "chattr", "ldconfig", "depmod", "update-initramfs",
        "dracut", "mkinitcpio", "init", "sysctl", "timedatectl",
        "hostnamectl", "localectl", "loginctl", "machinectl", "dpkg", "rpm",
        "docker", "podman", "crontab", "at", "insserv", "update-rc.d",
        "chkconfig", "setcap", "ethtool", NULL,
    };
    static const char *const writers[] = {
        "cp", "mv", "tee", "ln", "install", "touch", "mkdir", "rmdir", "rm",
        "truncate", "chmod", "chown", "chgrp", "rsync", "tar", "unzip",
        "sed", "curl", "wget", "patch", NULL,
    };
    static const char *const systemctl_mutations[] = {
        "start", "stop", "restart", "reload", "try-restart",
        "reload-or-restart", "enable", "disable", "reenable", "mask",
        "unmask", "daemon-reload", "daemon-reexec", "isolate", "kill",
        "reset-failed", "set-property", "set-default", "edit", "link",
        "revert", "preset", "suspend", "hibernate", "reboot", "kexec",
        "default", "rescue", "emergency", NULL,
    };
    static const char *const systemctl_off[] = {
        "poweroff", "halt", NULL,
    };
    static const char *const ip_mutations[] = {
        "set", "add", "del", "delete", "flush", "change", "replace",
        "append", NULL,
    };
    static const char *const read_flags_dpkg[] = {
        "-l", "--list", "-s", "--status", "-L", "--listfiles", "-S",
        "--search", "--version", NULL,
    };
    static const char *const read_docker[] = {
        "ps", "images", "version", "info", NULL,
    };
    static const char *const read_hostctl[] = {"status", NULL};

    if (strstr(raw, ":(){") || strstr(raw, ":() {")) {
        return SI_AGENT_ACTION_IRREVERSIBLE;
    }
    for (const char *p = strchr(raw, '>'); p; p = strchr(p + 1, '>')) {
        const char *target = p + 1;
        while (*target == '>' || *target == '|' || isspace((unsigned char)*target)) {
            target++;
        }
        if (strncmp(target, "/dev/", 5) == 0 &&
            strncmp(target, "/dev/null", 9) != 0) {
            return SI_AGENT_ACTION_IRREVERSIBLE;
        }
    }

    /* A wrapper (sudo, env, bash -c, xargs, find -exec, ...) can run any
     * later word of its segment, so every word there is a command word. */
    static const char *const runners[] = {
        "sudo", "doas", "su", "pkexec", "runuser", "env", "xargs", "nohup",
        "timeout", "nice", "ionice", "exec", "busybox", "sh", "bash", "zsh",
        "dash", "ksh", "eval", "watch", "time", "stdbuf", "setsid", "chroot",
        "find", "flock", "script", NULL,
    };
    unsigned runner_segments[CLASSIFY_MAX_TOKENS];
    size_t runner_count = 0;
    for (size_t i = 0; i < cmd->count; i++) {
        if (in_list(program_name(cmd->tokens[i].text), runners) &&
            runner_count < CLASSIFY_MAX_TOKENS) {
            runner_segments[runner_count++] = cmd->tokens[i].segment;
        }
    }

    bool system_hit = false;
    for (size_t i = 0; i < cmd->count; i++) {
        const char *token = cmd->tokens[i].text;
        const char *program = program_name(token);
        bool command_word = i == 0 ||
            cmd->tokens[i - 1].segment != cmd->tokens[i].segment;
        for (size_t r = 0; !command_word && r < runner_count; r++) {
            command_word = runner_segments[r] == cmd->tokens[i].segment;
        }
        if (starts_with(token, "of=/dev/") || starts_with(program, "mkfs")) {
            return SI_AGENT_ACTION_IRREVERSIBLE;
        }
        if (sensitive_token(token)) {
            system_hit = true;
        }
        if (cmd->tokens[i].after_redirect && system_path(token)) {
            system_hit = true;
        }
        if (!command_word) {
            continue;
        }
        if (in_list(program, irreversible)) {
            return SI_AGENT_ACTION_IRREVERSIBLE;
        }
        if (strcmp(program, "rm") == 0 && recursive_rm(cmd, i)) {
            return SI_AGENT_ACTION_IRREVERSIBLE;
        }
        if (strcmp(program, "systemctl") == 0) {
            if (segment_has(cmd, i, systemctl_off)) {
                return SI_AGENT_ACTION_IRREVERSIBLE;
            }
            if (segment_has(cmd, i, systemctl_mutations)) {
                system_hit = true;
            }
            continue;
        }
        if ((strcmp(program, "init") == 0 || strcmp(program, "telinit") == 0) &&
            i + 1 < cmd->count && strcmp(cmd->tokens[i + 1].text, "0") == 0) {
            return SI_AGENT_ACTION_IRREVERSIBLE;
        }
        if (strcmp(program, "ip") == 0) {
            if (segment_has(cmd, i, ip_mutations)) {
                system_hit = true;
            }
            continue;
        }
        if (strcmp(program, "journalctl") == 0) {
            if (segment_has_prefix(cmd, i, "--vacuum") ||
                segment_has_prefix(cmd, i, "--rotate") ||
                segment_has_prefix(cmd, i, "--flush")) {
                system_hit = true;
            }
            continue;
        }
        if (strcmp(program, "dmesg") == 0) {
            static const char *const clear[] = {"-c", "-C", "--clear",
                                                "--read-clear", NULL};
            if (segment_has(cmd, i, clear)) {
                system_hit = true;
            }
            continue;
        }
        if (strcmp(program, "dpkg") == 0 &&
            segment_has(cmd, i, read_flags_dpkg) && !cmd->control) {
            continue;
        }
        if (strcmp(program, "rpm") == 0 &&
            segment_has_prefix(cmd, i, "-q") && !cmd->control) {
            continue;
        }
        if ((strcmp(program, "docker") == 0 ||
             strcmp(program, "podman") == 0) &&
            i + 1 < cmd->count &&
            cmd->tokens[i + 1].segment == cmd->tokens[i].segment &&
            in_list(cmd->tokens[i + 1].text, read_docker)) {
            continue;
        }
        if ((strcmp(program, "hostnamectl") == 0 ||
             strcmp(program, "timedatectl") == 0) &&
            (i + 1 >= cmd->count ||
             cmd->tokens[i + 1].segment != cmd->tokens[i].segment ||
             in_list(cmd->tokens[i + 1].text, read_hostctl))) {
            continue;
        }
        if (strcmp(program, "sysctl") == 0 &&
            !segment_has(cmd, i, (const char *const[]){"-w", "-p",
                                                       "--write",
                                                       "--load", NULL}) &&
            !strchr(raw, '=')) {
            continue;
        }
        if (strcmp(program, "crontab") == 0 &&
            segment_has(cmd, i, (const char *const[]){"-l", NULL}) &&
            !segment_has(cmd, i, (const char *const[]){"-r", "-e", NULL})) {
            continue;
        }
        if (in_list(program, system)) {
            system_hit = true;
            continue;
        }
        if (in_list(program, writers)) {
            unsigned segment = cmd->tokens[i].segment;
            for (size_t j = i + 1; j < cmd->count &&
                 cmd->tokens[j].segment == segment; j++) {
                if (system_path(cmd->tokens[j].text)) {
                    system_hit = true;
                }
            }
        }
    }
    return system_hit ? SI_AGENT_ACTION_SYSTEM : SI_AGENT_ACTION_OPERATE;
}

static bool read_only_segment(const classify_command_t *cmd, size_t first)
{
    static const char *const programs[] = {
        "uname", "uptime", "whoami", "id", "df", "du", "free", "lsblk",
        "lscpu", "lsusb", "lspci", "lsmod", "lshw", "ls", "cat", "head",
        "tail", "wc", "stat", "file", "which", "whereis", "ps", "pgrep",
        "pidof", "pwd", "printenv", "nproc", "getent", "last", "lastlog",
        "w", "who", "groups", "ss", "netstat", "journalctl", "dmesg",
        "systemctl", "ip", "grep", "egrep", "fgrep", "zgrep", "sort",
        "uniq", "cut", "tr", "column", "find", "echo", "true", "sensors",
        "vmstat", "iostat", "mpstat", "lsof", "top", "hostname", "date",
        "hostnamectl", "timedatectl", "lsb_release", "sha256sum",
        "md5sum", "readlink", "realpath", "basename", "dirname", "nl",
        "dpkg", "rpm", "docker", "podman", "sysctl", "crontab", "env",
        NULL,
    };
    static const char *const systemctl_reads[] = {
        "status", "is-active", "is-enabled", "is-failed", "list-units",
        "list-unit-files", "list-timers", "list-sockets", "show", "cat",
        "list-dependencies", "--version", "--failed", NULL,
    };
    static const char *const find_effects[] = {
        "-delete", "-exec", "-execdir", "-ok", "-okdir", "-fprint",
        "-fprint0", "-fprintf", "-fls", NULL,
    };
    const char *program = program_name(cmd->tokens[first].text);
    if (!in_list(program, programs)) {
        return false;
    }
    unsigned segment = cmd->tokens[first].segment;
    size_t args = 0;
    for (size_t i = first + 1; i < cmd->count &&
         cmd->tokens[i].segment == segment; i++) {
        args++;
    }
    if (strcmp(program, "env") == 0) {
        return args == 0;
    }
    if (strcmp(program, "find") == 0) {
        return !segment_has(cmd, first, find_effects);
    }
    if (strcmp(program, "top") == 0) {
        return segment_has_prefix(cmd, first, "-b");
    }
    if (strcmp(program, "hostname") == 0) {
        for (size_t i = first + 1; i < cmd->count &&
             cmd->tokens[i].segment == segment; i++) {
            /* Input is lowercased, so -f/-F (fqdn vs. set-from-file) are
             * indistinguishable and both count as writes. */
            if (cmd->tokens[i].text[0] != '-' ||
                strcmp(cmd->tokens[i].text, "-b") == 0 ||
                strcmp(cmd->tokens[i].text, "-f") == 0 ||
                strcmp(cmd->tokens[i].text, "--file") == 0 ||
                strcmp(cmd->tokens[i].text, "--boot") == 0) {
                return false;
            }
        }
        return true;
    }
    if (strcmp(program, "date") == 0) {
        static const char *const set[] = {"-s", "--set", NULL};
        return !segment_has(cmd, first, set) &&
               !segment_has_prefix(cmd, first, "--set=");
    }
    if (strcmp(program, "systemctl") == 0) {
        return args == 0 || segment_has(cmd, first, systemctl_reads);
    }
    if (strcmp(program, "crontab") == 0) {
        static const char *const list[] = {"-l", NULL};
        return segment_has(cmd, first, list);
    }
    return true;
}

si_agent_action_class_t si_agent_classify_shell_command(const char *command)
{
    if (!command) {
        return SI_AGENT_ACTION_IRREVERSIBLE;
    }
    size_t len = strnlen(command, CLASSIFY_MAX_COMMAND + 1U);
    if (len == 0U || len > CLASSIFY_MAX_COMMAND) {
        return SI_AGENT_ACTION_IRREVERSIBLE;
    }
    char lowered[CLASSIFY_MAX_COMMAND + 1U];
    for (size_t i = 0; i < len; i++) {
        lowered[i] = (char)tolower((unsigned char)command[i]);
    }
    lowered[len] = '\0';

    /* ~13 KB: keep it off the small FreeRTOS task stacks. */
    classify_command_t *parsed = calloc(1, sizeof(*parsed));
    if (!parsed) {
        return SI_AGENT_ACTION_IRREVERSIBLE;
    }
    classify_parse(lowered, parsed);
    si_agent_action_class_t result = SI_AGENT_ACTION_READ;
    if (parsed->count == 0U) {
        result = SI_AGENT_ACTION_IRREVERSIBLE;
    } else if (parsed->truncated) {
        result = SI_AGENT_ACTION_SYSTEM;
    } else {
        result = classify_irreversible_or_system(lowered, parsed);
        if (result == SI_AGENT_ACTION_OPERATE && !parsed->control &&
            !parsed->redirect) {
            result = SI_AGENT_ACTION_READ;
            for (size_t i = 0; i < parsed->count; i++) {
                bool command_word = i == 0 ||
                    parsed->tokens[i - 1].segment != parsed->tokens[i].segment;
                if (command_word && !read_only_segment(parsed, i)) {
                    result = SI_AGENT_ACTION_OPERATE;
                    break;
                }
            }
        }
    }
    free(parsed);
    return result;
}

si_agent_action_class_t si_agent_classify_path_write(const char *path)
{
    if (!path || !path[0]) {
        return SI_AGENT_ACTION_IRREVERSIBLE;
    }
    char lowered[CLASSIFY_MAX_TOKEN];
    size_t len = strnlen(path, sizeof(lowered));
    if (len >= sizeof(lowered)) {
        return SI_AGENT_ACTION_SYSTEM;
    }
    for (size_t i = 0; i <= len; i++) {
        lowered[i] = (char)tolower((unsigned char)path[i]);
    }
    if (strstr(lowered, "..")) {
        return SI_AGENT_ACTION_SYSTEM;
    }
    if (starts_with(lowered, "/dev/")) {
        return strcmp(lowered, "/dev/null") == 0 ?
               SI_AGENT_ACTION_OPERATE : SI_AGENT_ACTION_IRREVERSIBLE;
    }
    if (system_path(lowered)) {
        return SI_AGENT_ACTION_SYSTEM;
    }
    if (lowered[0] != '/' || starts_with(lowered, "/home/") ||
        starts_with(lowered, "/tmp/") || starts_with(lowered, "/var/tmp/") ||
        starts_with(lowered, "~")) {
        return sensitive_token(lowered) ? SI_AGENT_ACTION_SYSTEM :
                                          SI_AGENT_ACTION_OPERATE;
    }
    return SI_AGENT_ACTION_SYSTEM;
}
