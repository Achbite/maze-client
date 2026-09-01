#!/usr/bin/env python3
"""Validate one attempt-private Infra training admission and publish its gate."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import stat


EXECUTION_KEYS = {
    "schema_version",
    "component",
    "component_attempt_id",
    "identity",
    "executable_generation",
}
IDENTITY_KEYS = {
    "allocation_id",
    "allocation_generation",
    "node_id",
    "node_session_id",
    "pod_attempt_id",
}
TOKEN_KEYS = {
    "schema_version",
    "admission_id",
    "admission_generation",
    "allocation_id",
    "allocation_generation",
    "node_id",
    "node_session_id",
    "pod_attempt_id",
    "executable_generation",
    "client_component_attempt_id",
}


def read_exact_object(path: Path, expected_keys: set[str], name: str) -> dict:
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_size <= 0 or info.st_size > 65536:
        raise ValueError(f"{name} must be one bounded regular file")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or set(value) != expected_keys:
        raise ValueError(f"{name} fields do not match the exact schema")
    return value


def identity_string(value: object, field: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or value != value.strip()
        or any(character in value for character in "\r\n\x00")
    ):
        raise ValueError(f"{field} is not a valid identity")
    return value


def positive_integer(value: object, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{field} must be a positive integer")
    return value


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--execution", required=True, type=Path)
    parser.add_argument("--token", required=True, type=Path)
    parser.add_argument("--marker", required=True, type=Path)
    arguments = parser.parse_args()

    execution = read_exact_object(arguments.execution, EXECUTION_KEYS, "execution receipt")
    if execution["schema_version"] != "rl.component-execution.v1":
        raise ValueError("execution receipt schema_version is incompatible")
    if execution["component"] != "client":
        raise ValueError("execution receipt does not belong to the client role")
    component_attempt_id = identity_string(
        execution["component_attempt_id"], "execution component_attempt_id"
    )
    executable_generation = positive_integer(
        execution["executable_generation"], "execution executable_generation"
    )
    identity = execution["identity"]
    if not isinstance(identity, dict) or set(identity) != IDENTITY_KEYS:
        raise ValueError("execution identity fields do not match the exact schema")
    normalized_identity = {
        "allocation_id": identity_string(identity["allocation_id"], "allocation_id"),
        "allocation_generation": positive_integer(
            identity["allocation_generation"], "allocation_generation"
        ),
        "node_id": identity_string(identity["node_id"], "node_id"),
        "node_session_id": identity_string(
            identity["node_session_id"], "node_session_id"
        ),
        "pod_attempt_id": identity_string(identity["pod_attempt_id"], "pod_attempt_id"),
    }

    token = read_exact_object(arguments.token, TOKEN_KEYS, "training admission token")
    if token["schema_version"] != "rl.training-admission.v1":
        raise ValueError("training admission token schema_version is incompatible")
    identity_string(token["admission_id"], "admission_id")
    positive_integer(token["admission_generation"], "admission_generation")
    if identity_string(
        token["client_component_attempt_id"], "client_component_attempt_id"
    ) != component_attempt_id:
        raise ValueError("training admission Client attempt does not match execution receipt")
    if positive_integer(
        token["executable_generation"], "token executable_generation"
    ) != executable_generation:
        raise ValueError("training admission executable generation does not match execution receipt")
    for field, expected in normalized_identity.items():
        actual = (
            positive_integer(token[field], f"token {field}")
            if field == "allocation_generation"
            else identity_string(token[field], f"token {field}")
        )
        if actual != expected:
            raise ValueError(f"training admission {field} does not match execution receipt")

    marker = {
        "schema_version": "rl.client-training-admitted.v1",
        "admission_id": token["admission_id"],
        "admission_generation": token["admission_generation"],
    }
    arguments.marker.parent.mkdir(parents=True, exist_ok=True)
    temporary = arguments.marker.with_name(
        arguments.marker.name + f".tmp.{os.getpid()}"
    )
    temporary.write_text(
        json.dumps(marker, ensure_ascii=False, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, arguments.marker)


if __name__ == "__main__":
    main()
