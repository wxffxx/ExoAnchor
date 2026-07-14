"""MCP tool schemas, annotations, and strict argument validation."""

from __future__ import annotations

from typing import Any

from .client import ExoAnchorError
from .observations import STATUS_CONDITIONS


JSON = dict[str, Any]


class ToolArgumentError(ExoAnchorError):
    pass


def _schema_object(properties: JSON, required: list[str] | None = None) -> JSON:
    schema: JSON = {
        "type": "object",
        "properties": properties,
        "additionalProperties": False,
    }
    if required:
        schema["required"] = required
    return schema


HID_KEY_CODES = [
    *[f"Key{letter}" for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"],
    *[f"Digit{digit}" for digit in "1234567890"],
    "Enter", "Escape", "Backspace", "Tab", "Space", "Minus", "Equal",
    "BracketLeft", "BracketRight", "Backslash", "Semicolon", "Quote",
    "Backquote", "Comma", "Period", "Slash", "CapsLock",
    *[f"F{number}" for number in range(1, 13)],
    "PrintScreen", "ScrollLock", "Pause", "Insert", "Home", "PageUp",
    "Delete", "End", "PageDown", "ArrowRight", "ArrowLeft", "ArrowDown",
    "ArrowUp", "NumLock", "NumpadDivide", "NumpadMultiply",
    "NumpadSubtract", "NumpadAdd", "NumpadEnter",
    *[f"Numpad{digit}" for digit in "1234567890"],
    "NumpadDecimal", "IntlBackslash", "ContextMenu", "Power",
]
HID_MODIFIER_CODES = [
    "ControlLeft", "ShiftLeft", "AltLeft", "MetaLeft",
    "ControlRight", "ShiftRight", "AltRight", "MetaRight",
]
HID_ACTION_TYPES = {
    "wait", "keydown", "keyup", "combo", "mousemove", "absmove",
    "absclick", "click", "wheel", "releaseall",
}
POWER_ACTIONS = {
    "power", "press_power", "reset", "press_reset", "force_off",
    "locator_on", "locator_off", "locator_toggle",
}
CONTROL_MODES = {"observe", "supervised", "autonomous"}
GENERIC_OUTPUT_SCHEMA: JSON = {"type": "object", "additionalProperties": True}
OBSERVATION_OUTPUT_SCHEMA: JSON = {
    "type": "object",
    "required": ["observation_id", "kind", "source", "captured_at", "trust", "content_sha256", "data"],
    "properties": {
        "observation_id": {"type": "string"},
        "kind": {"type": "string"},
        "source": {"type": "string"},
        "captured_at": {"type": "string"},
        "trust": {"const": "untrusted_external_observation"},
        "content_sha256": {"type": "string"},
        "data": {},
        "derived": {"type": "object"},
    },
    "additionalProperties": True,
}
SSH_OPERATIONS = {
    "system_summary",
    "disk_usage",
    "network_summary",
    "service_status",
    "command",
}
SSH_RECIPE_COMMANDS = {
    "system_summary": "uname -a; printf '\\n'; uptime; printf '\\n'; free -h 2>/dev/null || true",
    "disk_usage": "df -hP",
    "network_summary": "ip -brief address 2>/dev/null || ifconfig 2>/dev/null || true",
}


def _observation_tool_definition(name: str, title: str, description: str,
                                 input_schema: JSON) -> JSON:
    tool = _tool_definition(
        name,
        title,
        description,
        input_schema,
        read_only=True,
        destructive=False,
        idempotent=True,
        open_world=False,
    )
    tool["outputSchema"] = OBSERVATION_OUTPUT_SCHEMA
    return tool


def _hid_action_schema(action_type: str, properties: JSON | None = None,
                       required: list[str] | None = None) -> JSON:
    action_properties: JSON = {"type": {"const": action_type}}
    action_properties.update(properties or {})
    return _schema_object(action_properties, ["type", *(required or [])])


HID_ACTION_SCHEMA: JSON = {
    "oneOf": [
        _hid_action_schema(
            "wait",
            {"ms": {"type": "integer", "minimum": 0, "maximum": 5000}},
            ["ms"],
        ),
        _hid_action_schema(
            "keydown",
            {"code": {"type": "string", "enum": HID_KEY_CODES + HID_MODIFIER_CODES}},
            ["code"],
        ),
        _hid_action_schema(
            "keyup",
            {"code": {"type": "string", "enum": HID_KEY_CODES + HID_MODIFIER_CODES}},
            ["code"],
        ),
        _hid_action_schema(
            "combo",
            {
                "modifiers": {
                    "type": "array",
                    "maxItems": 8,
                    "uniqueItems": True,
                    "items": {"type": "string", "enum": HID_MODIFIER_CODES},
                },
                "keys": {
                    "type": "array",
                    "minItems": 1,
                    "maxItems": 6,
                    "uniqueItems": True,
                    "items": {"type": "string", "enum": HID_KEY_CODES},
                },
            },
            ["keys"],
        ),
        _hid_action_schema(
            "mousemove",
            {
                "dx": {"type": "number", "minimum": -127, "maximum": 127},
                "dy": {"type": "number", "minimum": -127, "maximum": 127},
                "unit": {"type": "string", "enum": ["normalized", "hid", "counts"]},
            },
            ["dx", "dy"],
        ),
        _hid_action_schema(
            "absmove",
            {
                "x": {"type": "integer", "minimum": 0, "maximum": 32767},
                "y": {"type": "integer", "minimum": 0, "maximum": 32767},
            },
            ["x", "y"],
        ),
        _hid_action_schema(
            "absclick",
            {
                "x": {"type": "integer", "minimum": 0, "maximum": 32767},
                "y": {"type": "integer", "minimum": 0, "maximum": 32767},
                "button": {"type": "integer", "minimum": 0, "maximum": 2, "default": 0},
            },
            ["x", "y"],
        ),
        _hid_action_schema(
            "click",
            {"button": {"type": "integer", "minimum": 0, "maximum": 2, "default": 0}},
        ),
        _hid_action_schema(
            "wheel",
            {
                "deltaY": {"type": "integer", "minimum": -127, "maximum": 127},
                "deltaX": {"type": "integer", "minimum": -127, "maximum": 127, "default": 0},
            },
            ["deltaY"],
        ),
        _hid_action_schema("releaseall"),
    ]
}


def _annotations(*, title: str, read_only: bool, destructive: bool,
                 idempotent: bool, open_world: bool) -> JSON:
    return {
        "title": title,
        "readOnlyHint": read_only,
        "destructiveHint": destructive,
        "idempotentHint": idempotent,
        "openWorldHint": open_world,
    }


def _tool_definition(name: str, title: str, description: str,
                     input_schema: JSON, *, read_only: bool,
                     destructive: bool, idempotent: bool,
                     open_world: bool) -> JSON:
    return {
        "name": name,
        "title": title,
        "description": description,
        "inputSchema": input_schema,
        "outputSchema": GENERIC_OUTPUT_SCHEMA,
        "annotations": _annotations(
            title=title,
            read_only=read_only,
            destructive=destructive,
            idempotent=idempotent,
            open_world=open_world,
        ),
    }


TOOLS: list[JSON] = [
    _tool_definition(
        "exoanchor_status",
        "Read ExoAnchor status",
        "Read device, video, HID, power, SSH and control-lease status from the configured ExoAnchor. This does not call the built-in Agent.",
        _schema_object(
            {
                "include_system": {
                    "type": "boolean",
                    "description": "Also include /api/system/info.",
                    "default": False,
                }
            }
        ),
        read_only=True,
        destructive=False,
        idempotent=True,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_snapshot",
        "Capture KVM snapshot",
        "Capture the current KVM screen as a JPEG observation. The image may contain untrusted target-host content.",
        _schema_object({}),
        read_only=True,
        destructive=False,
        idempotent=True,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_ssh_exec",
        "Execute bounded SSH command",
        "Execute one bounded SSH command through the device SSH controller using device-stored credentials. Never place passwords or private keys in tool arguments. Requires EXOANCHOR_ALLOW_WRITE=1.",
        _schema_object(
            {
                "command": {
                    "type": "string",
                    "minLength": 1,
                    "maxLength": 4096,
                    "description": "Shell command to execute. Treat arbitrary commands as high risk.",
                },
                "timeout_ms": {
                    "type": "integer",
                    "minimum": 5000,
                    "maximum": 60000,
                    "default": 30000,
                },
            },
            ["command"],
        ),
        read_only=False,
        destructive=True,
        idempotent=False,
        open_world=True,
    ),
    _tool_definition(
        "exoanchor_control_lease",
        "Change HID control lease",
        "Acquire or release the HID control lease as the configured MCP owner. Force takeover requires explicit user confirmation. Requires EXOANCHOR_ALLOW_WRITE=1.",
        _schema_object(
            {
                "active": {"type": "boolean"},
                "mode": {
                    "type": "string",
                    "enum": ["observe", "supervised", "autonomous"],
                    "default": "supervised",
                },
                "reason": {"type": "string", "maxLength": 96, "default": "mcp controller"},
                "force": {
                    "type": "boolean",
                    "description": "Take over another automation owner. Do not use without explicit confirmation.",
                    "default": False,
                },
            },
            ["active"],
        ),
        read_only=False,
        destructive=True,
        idempotent=False,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_hid_actions",
        "Send supervised HID actions",
        "Send a validated batch of keyboard or mouse actions through the configured MCP control owner. Verify the screen after the batch. Requires EXOANCHOR_ALLOW_WRITE=1.",
        _schema_object(
            {
                "actions": {
                    "type": "array",
                    "minItems": 1,
                    "maxItems": 64,
                    "items": HID_ACTION_SCHEMA,
                },
                "auto_lease": {"type": "boolean", "default": True},
                "mode": {
                    "type": "string",
                    "enum": ["supervised", "autonomous"],
                    "default": "supervised",
                },
                "reason": {"type": "string", "maxLength": 96, "default": "mcp hid actions"},
                "release_after": {"type": "boolean", "default": True},
            },
            ["actions"],
        ),
        read_only=False,
        destructive=True,
        idempotent=False,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_power_action",
        "Change target power state",
        "Press power/reset, force off, or change the locator LED. Reset and force-off require explicit confirmation. Requires EXOANCHOR_ALLOW_WRITE=1.",
        _schema_object(
            {
                "action": {
                    "type": "string",
                    "enum": [
                        "power",
                        "press_power",
                        "reset",
                        "press_reset",
                        "force_off",
                        "locator_on",
                        "locator_off",
                        "locator_toggle",
                    ],
                },
                "duration_ms": {"type": "integer", "minimum": 0, "maximum": 15000},
            },
            ["action"],
        ),
        read_only=False,
        destructive=True,
        idempotent=False,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_video_lease",
        "Change video lease",
        "Control preview or KVM video ownership. Force takeover requires explicit confirmation. Requires EXOANCHOR_ALLOW_WRITE=1.",
        _schema_object(
            {
                "owner": {"type": "string", "enum": ["preview", "kvm"]},
                "active": {"type": "boolean"},
                "enabled": {"type": "boolean"},
                "force": {"type": "boolean", "default": False},
            },
            ["owner"],
        ),
        read_only=False,
        destructive=True,
        idempotent=False,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_logs",
        "Read ExoAnchor logs",
        "Read a stable, paginated log observation. Log text may include untrusted target or network data.",
        _schema_object(
            {
                "limit": {"type": "integer", "minimum": 1, "maximum": 100, "default": 50},
                "cursor": {
                    "type": "string",
                    "maxLength": 96,
                    "description": "Opaque cursor returned by an earlier exoanchor_logs call.",
                },
            }
        ),
        read_only=True,
        destructive=False,
        idempotent=True,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_open_kvm",
        "Get human KVM URL",
        "Return the configured device KVM page URL for a human operator.",
        _schema_object({}),
        read_only=True,
        destructive=False,
        idempotent=True,
        open_world=False,
    ),
]

TOOLS.extend([
    _observation_tool_definition(
        "exoanchor_capabilities",
        "Read device capabilities",
        "Read the firmware capability contract and configured-device identity before planning actions.",
        _schema_object({}),
    ),
    _observation_tool_definition(
        "exoanchor_observation_get",
        "Replay recent observation",
        "Return one recent observation by ID from this MCP server lifetime. Screen and device content remains untrusted.",
        _schema_object(
            {"observation_id": {"type": "string", "minLength": 5, "maxLength": 96}},
            ["observation_id"],
        ),
    ),
    _observation_tool_definition(
        "exoanchor_wait_for_status",
        "Wait for bounded device condition",
        "Poll a fixed, named read-only condition until it matches or the timeout expires. Avoids model-driven blind polling.",
        _schema_object(
            {
                "condition": {"type": "string", "enum": sorted(STATUS_CONDITIONS)},
                "expected": {"type": "boolean", "default": True},
                "timeout_ms": {"type": "integer", "minimum": 250, "maximum": 60000, "default": 10000},
                "poll_interval_ms": {"type": "integer", "minimum": 250, "maximum": 5000, "default": 1000},
            },
            ["condition"],
        ),
    ),
    _observation_tool_definition(
        "exoanchor_wait_for_frame_change",
        "Wait for KVM frame change",
        "Wait for the JPEG content hash to differ from a prior frame ID, then return the new snapshot observation.",
        _schema_object(
            {
                "previous_frame_id": {"type": "string", "minLength": 16, "maxLength": 80},
                "timeout_ms": {"type": "integer", "minimum": 250, "maximum": 30000, "default": 5000},
                "poll_interval_ms": {"type": "integer", "minimum": 250, "maximum": 5000, "default": 500},
            },
            ["previous_frame_id"],
        ),
    ),
    _tool_definition(
        "exoanchor_click_pixel",
        "Click current KVM pixel",
        "Convert a pixel in the current snapshot to absolute HID coordinates. Requires an expected frame ID and rejects stale coordinates.",
        _schema_object(
            {
                "x": {"type": "integer", "minimum": 0, "maximum": 16384},
                "y": {"type": "integer", "minimum": 0, "maximum": 16384},
                "button": {"type": "integer", "minimum": 0, "maximum": 2, "default": 0},
                "expected_frame_id": {"type": "string", "minLength": 16, "maxLength": 80},
                "expected_device_id": {"type": "string", "minLength": 1, "maxLength": 128},
                "wait_after_ms": {"type": "integer", "minimum": 0, "maximum": 5000, "default": 250},
            },
            ["x", "y", "expected_frame_id"],
        ),
        read_only=False,
        destructive=True,
        idempotent=False,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_type_text",
        "Type bounded US-layout text",
        "Type printable ASCII text using an explicit US keyboard layout, then release every key and return a new observation.",
        _schema_object(
            {
                "text": {"type": "string", "minLength": 1, "maxLength": 256},
                "layout": {"const": "us"},
                "expected_frame_id": {"type": "string", "minLength": 16, "maxLength": 80},
                "expected_device_id": {"type": "string", "minLength": 1, "maxLength": 128},
                "inter_key_ms": {"type": "integer", "minimum": 0, "maximum": 500, "default": 20},
                "wait_after_ms": {"type": "integer", "minimum": 0, "maximum": 5000, "default": 250},
            },
            ["text", "layout", "expected_frame_id"],
        ),
        read_only=False,
        destructive=True,
        idempotent=False,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_execute_and_observe",
        "Execute bounded HID batch and observe",
        "Execute validated supervised HID actions against an expected frame, always release keys and the lease, then capture a verification observation.",
        _schema_object(
            {
                "actions": {"type": "array", "minItems": 1, "maxItems": 64, "items": HID_ACTION_SCHEMA},
                "expected_frame_id": {"type": "string", "minLength": 16, "maxLength": 80},
                "expected_device_id": {"type": "string", "minLength": 1, "maxLength": 128},
                "wait_after_ms": {"type": "integer", "minimum": 0, "maximum": 5000, "default": 250},
                "require_frame_change": {"type": "boolean", "default": False},
            },
            ["actions", "expected_frame_id"],
        ),
        read_only=False,
        destructive=True,
        idempotent=False,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_ssh_job_start",
        "Start bounded SSH job",
        "Start a bounded bridge-managed SSH operation using device-stored credentials. Unverified hosts and arbitrary commands are disabled by default.",
        _schema_object(
            {
                "operation": {"type": "string", "enum": sorted(SSH_OPERATIONS)},
                "service_name": {"type": "string", "pattern": "^[A-Za-z0-9_.@-]{1,96}$"},
                "command": {"type": "string", "minLength": 1, "maxLength": 2048},
                "secret_ref": {"const": "device-default"},
                "timeout_ms": {"type": "integer", "minimum": 5000, "maximum": 60000, "default": 30000},
                "idempotency_key": {"type": "string", "minLength": 8, "maxLength": 96},
                "expected_device_id": {"type": "string", "minLength": 1, "maxLength": 128},
            },
            ["operation", "secret_ref", "idempotency_key"],
        ),
        read_only=False,
        destructive=True,
        idempotent=True,
        open_world=True,
    ),
    _tool_definition(
        "exoanchor_ssh_job_status",
        "Read SSH job status",
        "Read bridge job state and audit metadata without starting remote work.",
        _schema_object({"job_id": {"type": "string", "minLength": 5, "maxLength": 96}}, ["job_id"]),
        read_only=True,
        destructive=False,
        idempotent=True,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_ssh_job_cancel",
        "Request SSH job cancellation",
        "Cancel queued work or mark an in-flight bounded request for cooperative cancellation. It cannot kill an HTTP request already executing on firmware.",
        _schema_object({"job_id": {"type": "string", "minLength": 5, "maxLength": 96}}, ["job_id"]),
        read_only=False,
        destructive=False,
        idempotent=True,
        open_world=False,
    ),
    _tool_definition(
        "exoanchor_ssh_job_result",
        "Read paginated SSH job result",
        "Read bounded SSH output with offset pagination and artifact hashes.",
        _schema_object(
            {
                "job_id": {"type": "string", "minLength": 5, "maxLength": 96},
                "offset": {"type": "integer", "minimum": 0, "maximum": 1000000, "default": 0},
                "limit": {"type": "integer", "minimum": 1, "maximum": 8192, "default": 4096},
            },
            ["job_id"],
        ),
        read_only=True,
        destructive=False,
        idempotent=True,
        open_world=False,
    ),
])

for _tool in TOOLS:
    if _tool["name"] in {
        "exoanchor_status", "exoanchor_snapshot", "exoanchor_logs",
        "exoanchor_capabilities", "exoanchor_observation_get",
        "exoanchor_wait_for_status", "exoanchor_wait_for_frame_change",
    }:
        _tool["outputSchema"] = OBSERVATION_OUTPUT_SCHEMA

TOOL_BY_NAME = {tool["name"]: tool for tool in TOOLS}


WRITE_TOOLS = {
    "exoanchor_ssh_exec",
    "exoanchor_control_lease",
    "exoanchor_hid_actions",
    "exoanchor_power_action",
    "exoanchor_video_lease",
    "exoanchor_click_pixel",
    "exoanchor_type_text",
    "exoanchor_execute_and_observe",
    "exoanchor_ssh_job_start",
    "exoanchor_ssh_job_cancel",
}


def _is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _validate_string(args: JSON, name: str, *, required: bool = False,
                     minimum: int = 0, maximum: int | None = None,
                     choices: set[str] | None = None) -> None:
    if name not in args:
        if required:
            raise ToolArgumentError(f"{name} is required")
        return
    value = args[name]
    if not isinstance(value, str):
        raise ToolArgumentError(f"{name} must be a string")
    if len(value) < minimum:
        raise ToolArgumentError(f"{name} is too short")
    if maximum is not None and len(value) > maximum:
        raise ToolArgumentError(f"{name} exceeds {maximum} characters")
    if choices is not None and value not in choices:
        raise ToolArgumentError(f"{name} must be one of {sorted(choices)}")


def _validate_bool(args: JSON, name: str, *, required: bool = False) -> None:
    if name not in args:
        if required:
            raise ToolArgumentError(f"{name} is required")
        return
    if not isinstance(args[name], bool):
        raise ToolArgumentError(f"{name} must be a boolean")


def _validate_integer(args: JSON, name: str, *, required: bool = False,
                      minimum: int | None = None,
                      maximum: int | None = None) -> None:
    if name not in args:
        if required:
            raise ToolArgumentError(f"{name} is required")
        return
    value = args[name]
    if not _is_integer(value):
        raise ToolArgumentError(f"{name} must be an integer")
    if minimum is not None and value < minimum:
        raise ToolArgumentError(f"{name} must be at least {minimum}")
    if maximum is not None and value > maximum:
        raise ToolArgumentError(f"{name} must be at most {maximum}")


def _validate_number_value(action: JSON, name: str, index: int,
                           minimum: float, maximum: float) -> None:
    value = action.get(name)
    if not _is_number(value) or value < minimum or value > maximum:
        raise ToolArgumentError(
            f"actions[{index}].{name} must be a number between {minimum} and {maximum}"
        )


def _validate_integer_value(action: JSON, name: str, index: int,
                            minimum: int, maximum: int) -> None:
    value = action.get(name)
    if not _is_integer(value) or value < minimum or value > maximum:
        raise ToolArgumentError(
            f"actions[{index}].{name} must be an integer between {minimum} and {maximum}"
        )


def _validate_hid_action(action: Any, index: int) -> None:
    if not isinstance(action, dict):
        raise ToolArgumentError(f"actions[{index}] must be an object")
    action_type = action.get("type")
    if not isinstance(action_type, str) or action_type not in HID_ACTION_TYPES:
        raise ToolArgumentError(
            f"actions[{index}].type must be one of {sorted(HID_ACTION_TYPES)}"
        )
    fields = {
        "wait": {"type", "ms"},
        "keydown": {"type", "code"},
        "keyup": {"type", "code"},
        "combo": {"type", "modifiers", "keys"},
        "mousemove": {"type", "dx", "dy", "unit"},
        "absmove": {"type", "x", "y"},
        "absclick": {"type", "x", "y", "button"},
        "click": {"type", "button"},
        "wheel": {"type", "deltaY", "deltaX"},
        "releaseall": {"type"},
    }[action_type]
    unknown = set(action) - fields
    if unknown:
        raise ToolArgumentError(
            f"actions[{index}] has unsupported fields: {sorted(unknown)}"
        )

    if action_type == "wait":
        _validate_integer_value(action, "ms", index, 0, 5000)
    elif action_type in {"keydown", "keyup"}:
        code = action.get("code")
        if code not in HID_KEY_CODES and code not in HID_MODIFIER_CODES:
            raise ToolArgumentError(f"actions[{index}].code is not a supported HID code")
    elif action_type == "combo":
        modifiers = action.get("modifiers", [])
        keys = action.get("keys")
        if (not isinstance(modifiers, list) or
                any(not isinstance(item, str) for item in modifiers) or
                len(modifiers) > 8 or len(set(modifiers)) != len(modifiers)):
            raise ToolArgumentError(f"actions[{index}].modifiers must be a unique array with at most 8 items")
        if any(item not in HID_MODIFIER_CODES for item in modifiers):
            raise ToolArgumentError(f"actions[{index}].modifiers contains an unsupported modifier")
        if (not isinstance(keys, list) or
                any(not isinstance(item, str) for item in keys) or
                not 1 <= len(keys) <= 6 or len(set(keys)) != len(keys)):
            raise ToolArgumentError(f"actions[{index}].keys must be a unique array with 1 to 6 items")
        if any(item not in HID_KEY_CODES for item in keys):
            raise ToolArgumentError(f"actions[{index}].keys contains an unsupported key")
    elif action_type == "mousemove":
        _validate_number_value(action, "dx", index, -127, 127)
        _validate_number_value(action, "dy", index, -127, 127)
        unit = action.get("unit", "normalized")
        if unit not in {"normalized", "hid", "counts"}:
            raise ToolArgumentError(f"actions[{index}].unit is invalid")
    elif action_type in {"absmove", "absclick"}:
        _validate_integer_value(action, "x", index, 0, 32767)
        _validate_integer_value(action, "y", index, 0, 32767)
        if "button" in action:
            _validate_integer_value(action, "button", index, 0, 2)
    elif action_type == "click":
        if "button" in action:
            _validate_integer_value(action, "button", index, 0, 2)
    elif action_type == "wheel":
        _validate_integer_value(action, "deltaY", index, -127, 127)
        if "deltaX" in action:
            _validate_integer_value(action, "deltaX", index, -127, 127)


def _validate_tool_arguments(name: str, args: JSON) -> None:
    tool = TOOL_BY_NAME.get(name)
    if tool is None:
        raise ToolArgumentError(f"unknown tool: {name}")
    allowed = set(tool["inputSchema"].get("properties", {}))
    unknown = set(args) - allowed
    if unknown:
        raise ToolArgumentError(f"unsupported arguments for {name}: {sorted(unknown)}")

    if name == "exoanchor_status":
        _validate_bool(args, "include_system")
    elif name in {"exoanchor_snapshot", "exoanchor_open_kvm", "exoanchor_capabilities"}:
        return
    elif name == "exoanchor_logs":
        _validate_integer(args, "limit", minimum=1, maximum=100)
        _validate_string(args, "cursor", maximum=96)
    elif name == "exoanchor_observation_get":
        _validate_string(args, "observation_id", required=True, minimum=5, maximum=96)
    elif name == "exoanchor_wait_for_status":
        _validate_string(args, "condition", required=True, choices=STATUS_CONDITIONS)
        _validate_bool(args, "expected")
        _validate_integer(args, "timeout_ms", minimum=250, maximum=60000)
        _validate_integer(args, "poll_interval_ms", minimum=250, maximum=5000)
    elif name == "exoanchor_wait_for_frame_change":
        _validate_string(args, "previous_frame_id", required=True, minimum=16, maximum=80)
        _validate_integer(args, "timeout_ms", minimum=250, maximum=30000)
        _validate_integer(args, "poll_interval_ms", minimum=250, maximum=5000)
    elif name == "exoanchor_ssh_exec":
        _validate_string(args, "command", required=True, minimum=1, maximum=4096)
        _validate_integer(args, "timeout_ms", minimum=5000, maximum=60000)
    elif name == "exoanchor_control_lease":
        _validate_bool(args, "active", required=True)
        _validate_string(args, "mode", choices=CONTROL_MODES)
        _validate_string(args, "reason", maximum=96)
        _validate_bool(args, "force")
    elif name == "exoanchor_hid_actions":
        actions = args.get("actions")
        if not isinstance(actions, list) or not 1 <= len(actions) <= 64:
            raise ToolArgumentError("actions must be an array with 1 to 64 items")
        for index, action in enumerate(actions):
            _validate_hid_action(action, index)
        _validate_bool(args, "auto_lease")
        _validate_string(args, "mode", choices={"supervised", "autonomous"})
        _validate_string(args, "reason", maximum=96)
        _validate_bool(args, "release_after")
    elif name == "exoanchor_power_action":
        _validate_string(args, "action", required=True, choices=POWER_ACTIONS)
        _validate_integer(args, "duration_ms", minimum=0, maximum=15000)
    elif name == "exoanchor_video_lease":
        _validate_string(args, "owner", required=True, choices={"preview", "kvm"})
        _validate_bool(args, "active")
        _validate_bool(args, "enabled")
        _validate_bool(args, "force")
        if args["owner"] == "preview":
            if "enabled" not in args or "active" in args or args.get("force"):
                raise ToolArgumentError("preview video lease requires enabled and does not accept active or force")
        elif "active" not in args or "enabled" in args:
            raise ToolArgumentError("kvm video lease requires active and does not accept enabled")
    elif name in {"exoanchor_click_pixel", "exoanchor_type_text", "exoanchor_execute_and_observe"}:
        _validate_string(args, "expected_frame_id", required=True, minimum=16, maximum=80)
        _validate_string(args, "expected_device_id", minimum=1, maximum=128)
        _validate_integer(args, "wait_after_ms", minimum=0, maximum=5000)
        if name == "exoanchor_click_pixel":
            _validate_integer(args, "x", required=True, minimum=0, maximum=16384)
            _validate_integer(args, "y", required=True, minimum=0, maximum=16384)
            _validate_integer(args, "button", minimum=0, maximum=2)
        elif name == "exoanchor_type_text":
            _validate_string(args, "text", required=True, minimum=1, maximum=256)
            _validate_string(args, "layout", required=True, choices={"us"})
            _validate_integer(args, "inter_key_ms", minimum=0, maximum=500)
            invalid = [char for char in args["text"] if char not in "\n\t" and not 32 <= ord(char) <= 126]
            if invalid:
                raise ToolArgumentError("text must contain printable ASCII, tab, or newline for layout=us")
        else:
            actions = args.get("actions")
            if not isinstance(actions, list) or not 1 <= len(actions) <= 64:
                raise ToolArgumentError("actions must be an array with 1 to 64 items")
            for index, action in enumerate(actions):
                _validate_hid_action(action, index)
            _validate_bool(args, "require_frame_change")
    elif name == "exoanchor_ssh_job_start":
        _validate_string(args, "operation", required=True, choices=SSH_OPERATIONS)
        _validate_string(args, "secret_ref", required=True, choices={"device-default"})
        _validate_string(args, "idempotency_key", required=True, minimum=8, maximum=96)
        _validate_string(args, "expected_device_id", minimum=1, maximum=128)
        _validate_integer(args, "timeout_ms", minimum=5000, maximum=60000)
        _validate_string(args, "service_name", minimum=1, maximum=96)
        _validate_string(args, "command", minimum=1, maximum=2048)
        operation = args["operation"]
        if operation == "service_status":
            service_name = args.get("service_name", "")
            if not service_name or any(
                char not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.@-"
                for char in service_name
            ):
                raise ToolArgumentError("service_status requires a safe service_name")
        elif "service_name" in args:
            raise ToolArgumentError("service_name is only valid for operation=service_status")
        if operation == "command" and "command" not in args:
            raise ToolArgumentError("operation=command requires command")
        if operation != "command" and "command" in args:
            raise ToolArgumentError("command is only valid for operation=command")
    elif name in {"exoanchor_ssh_job_status", "exoanchor_ssh_job_cancel"}:
        _validate_string(args, "job_id", required=True, minimum=5, maximum=96)
    elif name == "exoanchor_ssh_job_result":
        _validate_string(args, "job_id", required=True, minimum=5, maximum=96)
        _validate_integer(args, "offset", minimum=0, maximum=1000000)
        _validate_integer(args, "limit", minimum=1, maximum=8192)


US_SHIFTED_DIGITS = {
    "!": "Digit1", "@": "Digit2", "#": "Digit3", "$": "Digit4", "%": "Digit5",
    "^": "Digit6", "&": "Digit7", "*": "Digit8", "(": "Digit9", ")": "Digit0",
}
US_PUNCTUATION = {
    "-": ("Minus", False), "_": ("Minus", True),
    "=": ("Equal", False), "+": ("Equal", True),
    "[": ("BracketLeft", False), "{": ("BracketLeft", True),
    "]": ("BracketRight", False), "}": ("BracketRight", True),
    "\\": ("Backslash", False), "|": ("Backslash", True),
    ";": ("Semicolon", False), ":": ("Semicolon", True),
    "'": ("Quote", False), '"': ("Quote", True),
    "`": ("Backquote", False), "~": ("Backquote", True),
    ",": ("Comma", False), "<": ("Comma", True),
    ".": ("Period", False), ">": ("Period", True),
    "/": ("Slash", False), "?": ("Slash", True),
}


def _us_key_for_character(character: str) -> tuple[str, bool]:
    if "a" <= character <= "z":
        return f"Key{character.upper()}", False
    if "A" <= character <= "Z":
        return f"Key{character}", True
    if "0" <= character <= "9":
        return f"Digit{character}", False
    if character in US_SHIFTED_DIGITS:
        return US_SHIFTED_DIGITS[character], True
    if character in US_PUNCTUATION:
        return US_PUNCTUATION[character]
    if character == " ":
        return "Space", False
    if character == "\n":
        return "Enter", False
    if character == "\t":
        return "Tab", False
    raise ToolArgumentError(f"character {character!r} is not supported by layout=us")


def _text_to_hid_actions(text: str, inter_key_ms: int) -> list[JSON]:
    actions: list[JSON] = []
    for character in text:
        key, shift = _us_key_for_character(character)
        action: JSON = {"type": "combo", "keys": [key]}
        if shift:
            action["modifiers"] = ["ShiftLeft"]
        actions.append(action)
        if inter_key_ms:
            actions.append({"type": "wait", "ms": inter_key_ms})
    return actions
