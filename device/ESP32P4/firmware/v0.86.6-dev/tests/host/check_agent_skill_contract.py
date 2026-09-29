#!/usr/bin/env python3
from pathlib import Path
import re
import sys


PROJECT = Path(__file__).resolve().parents[2]
SKILL_ROOT = PROJECT / "main" / "agent_skills"
SERVICE = PROJECT / "main" / "application" / "agent_skill_service.c"
SERVICE_HEADER = PROJECT / "main" / "application" / "agent_skill_service.h"
REQUEST = PROJECT / "main" / "services" / "web" / "agent_request_module.inc"
SETTINGS = (
    PROJECT / "main" / "services" / "web" / "base_settings_http_module.inc"
)
SKILL_REGISTRY = (
    PROJECT / "main" / "services" / "web" / "agent_skill_registry_module.inc"
)
CMAKE = PROJECT / "main" / "CMakeLists.txt"
ROUTER = PROJECT / "main" / "core" / "agent_router.c"
WEB_SERVER = PROJECT / "main" / "services" / "web_server.c"

SKILLS = {
    "configure-uart-cli": (
        "uart_status",
        "uart_read",
        "manual_terminal_connected=true",
        "Never send passwords",
        "serial-getty@ttyACM0.service.d/override.conf",
        "ExecStart=",
        "--keep-baud 115200,9600",
        "stty -F /dev/ttyACM0 115200",
        "does not bypass tool policy",
        "pending_bytes=0",
    ),
    "configure-access-via-kvm": (
        "observe_video_status",
        "observe_screenshot",
        "observe_hid_status",
        "console_login",
        "Never disable the only working access path",
        "serial-getty@ttyACM0.service.d/override.conf",
        "openssh-server",
        "EXOANCHOR_SSH_OK",
        "does not bypass tool policy",
    ),
    "bridge-uart-ssh-access": (
        "Preserve the currently working channel",
        "manual_terminal_connected=true",
        "Use SSH to establish UART",
        "Use UART to establish SSH",
        "append_enter=true",
        "pending_bytes=0",
        "serial-getty@ttyACM0.service.d/override.conf",
        "EXOANCHOR_SSH_OK",
        "does not bypass tool policy",
    ),
}


def fail(message: str) -> None:
    print(f"agent skill contract: FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def validate_skill(name: str, markers: tuple[str, ...]) -> None:
    skill = SKILL_ROOT / name / "SKILL.md"
    openai_yaml = skill.parent / "agents" / "openai.yaml"
    for path in (skill, openai_yaml):
        if not path.exists():
            fail(f"missing {path}")

    raw = skill.read_bytes()
    if len(raw) > 8192:
        fail(f"{name}/SKILL.md exceeds the firmware injection bound")
    text = raw.decode("utf-8")
    match = re.match(r"^---\n(.*?)\n---\n", text, re.S)
    if not match:
        fail(f"{name}/SKILL.md frontmatter is missing")
    fields = [
        line.split(":", 1)[0].strip()
        for line in match.group(1).splitlines()
        if ":" in line
    ]
    if fields != ["name", "description"]:
        fail(f"{name} frontmatter fields must be name,description; got {fields}")
    if f"name: {name}" not in text:
        fail(f"{name}/SKILL.md has the wrong name")
    for marker in markers:
        if marker not in text:
            fail(f"{name}/SKILL.md missing {marker!r}")
    if "TODO" in text:
        fail(f"{name}/SKILL.md still contains TODO placeholders")

    openai = openai_yaml.read_text(encoding="utf-8")
    if f"${name}" not in openai:
        fail(f"{name}/agents/openai.yaml does not reference the skill")


def main() -> int:
    for path in (
        SERVICE,
        SERVICE_HEADER,
        REQUEST,
        SETTINGS,
        SKILL_REGISTRY,
        CMAKE,
        ROUTER,
        WEB_SERVER,
    ):
        if not path.exists():
            fail(f"missing {path}")
    for name, markers in SKILLS.items():
        validate_skill(name, markers)

    cmake = CMAKE.read_text(encoding="utf-8")
    for marker in (
        '"application/agent_skill_service.c"',
        "agent_skills/configure-uart-cli/SKILL.md",
        "agent_skills/configure-access-via-kvm/SKILL.md",
        "agent_skills/bridge-uart-ssh-access/SKILL.md",
        "RENAME_TO configure_uart_cli_skill",
        "RENAME_TO configure_access_via_kvm_skill",
        "RENAME_TO bridge_uart_ssh_access_skill",
    ):
        if marker not in cmake:
            fail(f"CMake missing {marker}")

    service_header = SERVICE_HEADER.read_text(encoding="utf-8")
    for marker in (
        "SI_AGENT_SKILL_CONFIGURE_UART_CLI",
        "SI_AGENT_SKILL_CONFIGURE_ACCESS_VIA_KVM",
        "SI_AGENT_SKILL_BRIDGE_UART_SSH_ACCESS",
        "si_agent_skill_count",
        "si_agent_skill_name_at",
    ):
        if marker not in service_header:
            fail(f"skill service header missing {marker}")

    service = SERVICE.read_text(encoding="utf-8")
    for marker in (
        '"_binary_configure_uart_cli_skill_start"',
        '"_binary_configure_access_via_kvm_skill_start"',
        '"_binary_bridge_uart_ssh_access_skill_start"',
        "SI_AGENT_TOOLS_SKILLS_MAX_LEN",
        "enabled = !cJSON_IsFalse",
        "SI_AGENT_ROUTE_KVM_VISUAL",
        "SI_AGENT_ROUTE_UART_OPS",
        "SI_AGENT_ROUTE_SSH_OPS",
        '"通过ssh配置uart"',
        '"通过uart配置ssh"',
        "length > 8192U",
    ):
        if marker not in service:
            fail(f"skill service missing {marker}")

    request = REQUEST.read_text(encoding="utf-8")
    if request.count("agent_add_builtin_skill_context(messages, route, message);") != 3:
        fail("chat, JSON repair, and continue builders do not all inject skills")
    for marker in (
        "si_agent_skill_count()",
        "si_agent_skill_name_at(index)",
        "Active built-in firmware Skill: %s.",
        "do not treat it ",
        "as authorization: tool policy",
        "si_agent_skill_enabled",
        "si_agent_skill_should_activate",
    ):
        if marker not in request:
            fail(f"request skill injection missing {marker}")

    settings = SETTINGS.read_text(encoding="utf-8")
    skill_registry = SKILL_REGISTRY.read_text(encoding="utf-8")
    for marker in (
        "agent_skills_parse_with_defaults",
    ):
        if marker not in settings:
            fail(f"saved skill registry response path missing {marker}")
    for marker in (
        "agent_skills_merge_missing_defaults",
        "cJSON_Duplicate(item, true)",
        "AGENT_TOOLS_SKILLS_DEFAULT",
        'strcmp(source->valuestring, "firmware")',
        "strlen(merged) < skills_size",
    ):
        if marker not in skill_registry:
            fail(f"saved skill registry upgrade path missing {marker}")

    router = ROUTER.read_text(encoding="utf-8")
    for marker in (
        '"through kvm"',
        '"通过uart"',
        '"通过ssh"',
        "bootstrap_uart_or_ssh_through_kvm",
        "bootstrap_ssh_through_target_uart",
        "bootstrap_uart_through_target_ssh",
    ):
        if marker not in router:
            fail(f"source-channel router contract missing {marker}")

    web_server = WEB_SERVER.read_text(encoding="utf-8")
    for name in SKILLS:
        marker = f'\\\"name\\\":\\\"{name}\\\"'
        if marker not in web_server:
            fail(f"default skill registry missing {name}")
    if '\\"source\\":\\"firmware\\"' not in web_server:
        fail("default skill registry does not identify firmware skills")

    print("agent skill contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
