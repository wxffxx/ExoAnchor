"""Consistent JSON nesting limits across supported Python versions."""
from __future__ import annotations

import json


MAX_JSON_DEPTH = 64


def loads_bounded_json(content: str | bytes) -> object:
    """Decode JSON, then bound container depth before consumers use the value.

    Input-byte limits and translation of parser errors belong to the caller.
    Do not rely on the interpreter's recursion limit: it varies between
    Python releases. Iterator frames keep this walk nonrecursive and avoid an
    additional list proportional to the width of the document.
    """
    value = json.loads(content)
    exhausted = object()
    stack = [iter((value,))]
    while stack:
        item = next(stack[-1], exhausted)
        if item is exhausted:
            stack.pop()
        elif isinstance(item, (dict, list)):
            if len(stack) > MAX_JSON_DEPTH:
                raise ValueError(f"JSON nesting exceeds {MAX_JSON_DEPTH} levels")
            stack.append(iter(item.values() if isinstance(item, dict) else item))
    return value
