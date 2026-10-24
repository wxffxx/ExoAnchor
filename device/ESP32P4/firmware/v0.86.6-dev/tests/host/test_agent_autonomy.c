#include "agent_autonomy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define R0 SI_AGENT_ACTION_READ
#define R1 SI_AGENT_ACTION_OPERATE
#define R2 SI_AGENT_ACTION_SYSTEM
#define R3 SI_AGENT_ACTION_IRREVERSIBLE

static int failures;

static void expect_command(const char *command, si_agent_action_class_t want)
{
    si_agent_action_class_t got = si_agent_classify_shell_command(command);
    if (got != want) {
        fprintf(stderr, "classify(%s): got %s, want %s\n",
                command ? command : "(null)",
                si_agent_action_class_name(got),
                si_agent_action_class_name(want));
        failures++;
    }
}

static void expect_path(const char *path, si_agent_action_class_t want)
{
    si_agent_action_class_t got = si_agent_classify_path_write(path);
    if (got != want) {
        fprintf(stderr, "path(%s): got %s, want %s\n", path ? path : "(null)",
                si_agent_action_class_name(got),
                si_agent_action_class_name(want));
        failures++;
    }
}

static void matrix(void)
{
    /* allows / auto per level, columns R0..R3 */
    static const struct {
        si_agent_autonomy_level_t level;
        bool allows[4];
        bool automatic[4];
    } rows[] = {
        {SI_AGENT_AUTONOMY_L0, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {SI_AGENT_AUTONOMY_L1, {1, 0, 0, 0}, {1, 0, 0, 0}},
        {SI_AGENT_AUTONOMY_L2, {1, 1, 0, 0}, {1, 0, 0, 0}},
        {SI_AGENT_AUTONOMY_L3, {1, 1, 1, 0}, {1, 1, 0, 0}},
        {SI_AGENT_AUTONOMY_L4, {1, 1, 1, 1}, {1, 1, 1, 0}},
    };
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        for (int c = 0; c < 4; c++) {
            assert(si_agent_autonomy_allows(rows[r].level,
                                            (si_agent_action_class_t)c) ==
                   rows[r].allows[c]);
            assert(si_agent_autonomy_auto_approves(
                       rows[r].level, (si_agent_action_class_t)c) ==
                   rows[r].automatic[c]);
        }
    }
    assert(si_agent_autonomy_auto_through(SI_AGENT_AUTONOMY_L0) == -1);
    assert(si_agent_autonomy_auto_through(SI_AGENT_AUTONOMY_L4) == (int)R2);
    /* L5 is not offered and out-of-range levels admit nothing. */
    assert(!si_agent_autonomy_level_valid(5));
    assert(!si_agent_autonomy_level_valid(-1));
    assert(!si_agent_autonomy_allows((si_agent_autonomy_level_t)5, R0));
    assert(SI_AGENT_AUTONOMY_DEFAULT == SI_AGENT_AUTONOMY_L1);
}

int main(void)
{
    matrix();

    /* R0: provably read-only; output goes to the cloud model. */
    expect_command("uname -a", R0);
    expect_command("  uptime ", R0);
    expect_command("df -h", R0);
    expect_command("free -m", R0);
    expect_command("cat /etc/os-release", R0);
    expect_command("systemctl status nginx", R0);
    expect_command("systemctl is-active sshd", R0);
    expect_command("journalctl -u nginx -n 50 --no-pager", R0);
    expect_command("ps aux | grep java", R0);
    expect_command("ls -la /var/www", R0);
    expect_command("ip addr show", R0);
    expect_command("ss -tlnp", R0);
    expect_command("hostname", R0);
    expect_command("dpkg -l nginx", R0);
    expect_command("docker ps", R0);
    expect_command("top -bn1", R0);
    expect_command("crontab -l", R0);

    /* Not provably read-only => at least R1. */
    expect_command("top", R1);
    expect_command("echo hi > notes.txt", R1);
    expect_command("uname; whoami", R1);
    expect_command("cat $(which ls)", R1);
    expect_command("mkdir -p ~/app", R1);
    expect_command("rm old.log", R1);
    expect_command("touch /tmp/marker", R1);
    expect_command("python3 script.py", R1);
    expect_command("find . -name '*.tmp'", R0);
    expect_command("find /tmp -name x -delete", R1);
    expect_command("git pull", R1);
    expect_command("hostname newname", R1);
    expect_command("hostname -F /tmp/name", R1);
    expect_command("date -s 2020-01-01", R1);

    /* R2: privilege, services, packages, users, network, system paths,
     * and any read of credential material. */
    expect_command("sudo systemctl restart nginx", R2);
    expect_command("systemctl restart nginx", R2);
    expect_command("systemctl enable --now docker", R2);
    expect_command("apt-get install -y nginx", R2);
    expect_command("useradd -m deploy", R2);
    expect_command("ip link set eth0 down", R2);
    expect_command("reboot", R2);
    expect_command("kill -9 1234", R2);
    expect_command("cp app.conf /etc/nginx/conf.d/", R2);
    expect_command("echo x >> /etc/hosts", R2);
    expect_command("cat /etc/shadow", R2);
    expect_command("cat ~/.ssh/id_ed25519", R2);
    expect_command("cat .env", R2);
    expect_command("docker run --privileged alpine", R2);
    expect_command("sysctl -w net.ipv4.ip_forward=1", R2);
    expect_command("crontab -r", R2);
    expect_command("chmod 600 /root/x", R2);

    /* R3: destructive, recursive deletion, power-off. */
    expect_command("rm -rf /var/www/old", R3);
    expect_command("rm -fr build", R3);
    expect_command("rm --recursive tmp", R3);
    expect_command("sudo rm -rf /", R3);
    expect_command("sudo -u root rm -r /srv/data", R3);
    expect_command("bash -c \"rm -rf ~\"", R3);
    expect_command("find / -name '*.log' -exec rm -rf {} +", R3);
    expect_command("dd if=/dev/zero of=/dev/sda bs=1M", R3);
    expect_command("mkfs.ext4 /dev/sdb1", R3);
    expect_command("wipefs -a /dev/nvme0n1", R3);
    expect_command("shutdown -h now", R3);
    expect_command("sudo poweroff", R3);
    expect_command("systemctl poweroff", R3);
    expect_command("echo 1 > /dev/sda", R3);
    expect_command(":(){ :|:& };:", R3);
    expect_command("", R3);
    expect_command("   ", R3);
    expect_command(NULL, R3);

    /* Writes to /dev/null are harmless; quoting does not hide words. */
    expect_command("ls > /dev/null", R1);
    expect_command("echo 'sudo is not run here'", R2);

    expect_path("notes.txt", R1);
    expect_path("/home/deploy/app/config.yml", R1);
    expect_path("/tmp/payload.bin", R1);
    expect_path("/etc/nginx/nginx.conf", R2);
    expect_path("/usr/local/bin/tool", R2);
    expect_path("/opt/app", R2);
    expect_path("/srv/www", R2);
    expect_path("/home/deploy/.ssh/authorized_keys", R2);
    expect_path("/home/deploy/../../etc/passwd", R2);
    expect_path("/", R2);
    expect_path("/dev/sda", R3);
    expect_path("/dev/null", R1);
    expect_path("", R3);
    expect_path(NULL, R3);

    if (failures) {
        fprintf(stderr, "agent autonomy tests: %d FAILED\n", failures);
        return 1;
    }
    puts("agent autonomy tests: PASS");
    return 0;
}
