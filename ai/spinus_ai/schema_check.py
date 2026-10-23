"""Checks a proposal step's parameters against the command's JSON schema (the subset commands.schema.json
uses), so the model hears about mistakes and fixes them before the user sees the proposal."""

from __future__ import annotations

from typing import Any

TYPES: dict[str, tuple[type, ...]] = {
    "object": (dict,),
    "array": (list,),
    "string": (str,),
    "boolean": (bool,),
    "integer": (int,),
    "number": (int, float),
}


def is_reference(value: Any) -> bool:
    """ "$2.channel": an earlier step's result, filled in by Spinus."""
    return isinstance(value, str) and value.startswith("$") and "." in value


def problems(value: Any, schema: dict[str, Any], path: str = "parameters") -> list[str]:
    if is_reference(value):
        return []
    found: list[str] = []
    expected = schema.get("type")
    if isinstance(expected, str) and expected in TYPES:
        ok = isinstance(value, TYPES[expected]) and not (expected in ("integer", "number") and isinstance(value, bool))
        if expected == "integer" and isinstance(value, float) and value.is_integer():
            ok = True
        if not ok:
            return [f"{path} must be {'an' if expected[0] in 'aeiou' else 'a'} {expected}"]
    if "enum" in schema and value not in schema["enum"]:
        found.append(f"{path} must be one of {schema['enum']}")
    if isinstance(value, int | float) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            found.append(f"{path} must be at least {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            found.append(f"{path} must be at most {schema['maximum']}")
    if isinstance(value, dict):
        properties: dict[str, Any] = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in value:
                found.append(f"{path}.{name} is missing")
        for name, item in value.items():
            if name in properties:
                found.extend(problems(item, properties[name], f"{path}.{name}"))
            elif schema.get("additionalProperties") is False:
                found.append(f"{path}.{name} is not a parameter (known: {', '.join(properties) or 'none'})")
            elif isinstance(schema.get("additionalProperties"), dict):  # a map: each value by one schema
                found.extend(problems(item, schema["additionalProperties"], f"{path}.{name}"))
    if isinstance(value, list) and isinstance(schema.get("items"), dict):
        for i, item in enumerate(value):
            found.extend(problems(item, schema["items"], f"{path}[{i}]"))
    return found
