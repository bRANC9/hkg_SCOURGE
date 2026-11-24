# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Scenario screenshot checkpoints: validation, trigger matching and capture.

A checkpoint is host-side evidence.  The guest keeps printing its normal
checks and markers; while the command runs, the watcher reads the serial log
and asks QMP for a PNG when a configured trigger appears.  Captures never
touch guest state and are recorded in ``result.json``.
"""
from __future__ import annotations

import math
import re
import threading
import time
from pathlib import Path
from typing import Callable

from .qmp import screendump
from .screenshots import nonblack_bbox

NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
MAX_CHECKPOINTS = 16
TRIGGERS = ("after_check", "after_marker", "after_seconds")
_TAIL_BYTES = 1024 * 1024


def validate_checkpoints(value) -> list[str]:
    """Validate a scenario ``screenshots`` list; return contract problems."""
    if value is None:
        return []
    if not isinstance(value, list):
        return ["screenshots must be a list"]
    problems: list[str] = []
    if len(value) > MAX_CHECKPOINTS:
        problems.append(
            f"screenshots must contain at most {MAX_CHECKPOINTS} entries")
    names: set[str] = set()
    for index, entry in enumerate(value):
        prefix = f"screenshots[{index}]"
        if not isinstance(entry, dict):
            problems.append(f"{prefix} must be an object")
            continue
        name = entry.get("name")
        if not isinstance(name, str) or not NAME_RE.fullmatch(name):
            problems.append(f"{prefix}.name must be a safe non-empty name")
        elif name in names:
            problems.append(f"{prefix}.name is duplicated: {name}")
        else:
            names.add(name)
        triggers = [key for key in TRIGGERS if key in entry]
        if len(triggers) != 1:
            problems.append(
                f"{prefix} must have exactly one of " + ", ".join(TRIGGERS))
        for trigger in triggers:
            trigger_value = entry[trigger]
            if trigger == "after_seconds":
                if (not isinstance(trigger_value, (int, float))
                        or isinstance(trigger_value, bool)
                        or not math.isfinite(trigger_value)
                        or trigger_value < 0):
                    problems.append(
                        f"{prefix}.after_seconds must be a non-negative "
                        "finite number")
            else:
                if (not isinstance(trigger_value, str)
                        or not trigger_value.strip()):
                    problems.append(
                        f"{prefix}.{trigger} must be a non-empty string")
                elif (trigger == "after_check"
                        and not NAME_RE.fullmatch(trigger_value)):
                    problems.append(
                        f"{prefix}.after_check must be a safe check name")
                elif (trigger == "after_marker"
                        and ("\n" in trigger_value or "\r" in trigger_value
                             or len(trigger_value) > 200)):
                    problems.append(
                        f"{prefix}.after_marker must be one line of at most "
                        "200 characters")
        delay = entry.get("delay", 0)
        if (not isinstance(delay, (int, float)) or isinstance(delay, bool)
                or not math.isfinite(delay) or delay < 0):
            problems.append(
                f"{prefix}.delay must be a non-negative finite number")
        for key in ("required", "nonblank"):
            if key in entry and not isinstance(entry[key], bool):
                problems.append(f"{prefix}.{key} must be boolean")
    return problems


def normalize_checkpoints(value) -> list[dict]:
    """Return a validated copy of ``value`` or raise ``ValueError``."""
    problems = validate_checkpoints(value)
    if problems:
        raise ValueError("; ".join(problems))
    return [dict(entry) for entry in (value or [])]


def check_pattern(name: str) -> re.Pattern[str]:
    return re.compile(rf"(?m)^HARNESS_CHECK {re.escape(name)} PASS\r?$")


def marker_pattern(marker: str) -> re.Pattern[str]:
    return re.compile(rf"(?m)^{re.escape(marker)}\r?$")


def required_failures(entries: list[dict]) -> list[str]:
    """Return FAIL reasons for missing required or blank non-blank checkpoints."""
    problems: list[str] = []
    for entry in entries:
        name = entry.get("name", "checkpoint")
        required = bool(entry.get("required"))
        missed = bool(entry.get("missed"))
        error = entry.get("error")
        if required and missed:
            problems.append(f"checkpoint {name} was not triggered")
            continue
        if required and error:
            problems.append(f"checkpoint {name} failed: {error}")
            continue
        if entry.get("nonblank") and not missed and not error:
            pixels = entry.get("pixels")
            if pixels is None:
                problems.append(
                    f"checkpoint {name} could not verify a visible frame")
            elif not pixels.get("bbox"):
                problems.append(f"checkpoint {name} is blank")
    return problems


class CheckpointWatcher(threading.Thread):
    """Capture QMP PNGs while a guest command runs.

    ``serial`` is the harness Serial (or a test double with ``log``,
    ``cond`` and ``eof``).  ``capture`` is injectable so tests never need a
    real QEMU instance.
    """

    def __init__(self, serial, socket_path: Path, directory: Path,
                 checkpoints: list[dict], *,
                 capture: Callable = screendump, pixels: bool = True,
                 poll: float = 0.1):
        super().__init__(name="vm-checkpoint-watcher", daemon=True)
        self.serial = serial
        self.socket_path = Path(socket_path)
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.capture = capture
        self.pixels = pixels
        self.poll = max(0.02, float(poll))
        self.stop_event = threading.Event()
        with self.serial.cond:
            self._offset = len(self.serial.log)
        self._started_at = time.monotonic()
        self._text = ""
        self._pending: dict[int, dict] = {}
        self._entries: dict[int, dict] = {}
        self._lock = threading.Lock()
        self.checkpoints = []
        for item in checkpoints:
            prepared = dict(item)
            prepared["_check"] = (check_pattern(item["after_check"])
                                  if "after_check" in item else None)
            prepared["_marker"] = (marker_pattern(item["after_marker"])
                                   if "after_marker" in item else None)
            self.checkpoints.append(prepared)

    def _drain(self) -> bool:
        with self.serial.cond:
            chunk = bytes(self.serial.log[self._offset:])
            self._offset = len(self.serial.log)
            eof = bool(getattr(self.serial, "eof", False))
        if chunk:
            self._text += chunk.decode("utf-8", "replace")
            if len(self._text) > _TAIL_BYTES:
                self._text = self._text[-_TAIL_BYTES:]
        return eof

    def _capture(self, index: int, now: float, trigger: str,
                 late: float | None = None) -> None:
        item = self.checkpoints[index]
        name = item["name"]
        output = self.directory / f"{name}.png"
        entry = {
            "name": name,
            "trigger": trigger,
            "seconds": round(now - self._started_at, 3),
            "required": bool(item.get("required", False)),
            "nonblank": bool(item.get("nonblank", False)),
        }
        if late is not None:
            entry["delay_seconds"] = round(late, 3)
        try:
            metadata = self.capture(self.socket_path, output)
        except Exception as exc:  # noqa: BLE001
            entry["error"] = f"{type(exc).__name__}: {exc}"
        else:
            entry.update({
                "path": metadata["path"],
                "size": metadata["size"],
                "sha256": metadata["sha256"],
                "width": metadata["width"],
                "height": metadata["height"],
            })
            if self.pixels or item.get("nonblank"):
                try:
                    entry["pixels"] = nonblack_bbox(output)
                except Exception as exc:  # noqa: BLE001
                    entry["pixel_error"] = f"{type(exc).__name__}: {exc}"
        self._entries[index] = entry

    def _scan(self, now: float, final: bool = False) -> None:
        elapsed = now - self._started_at
        with self._lock:
            for index, item in enumerate(self.checkpoints):
                if index in self._entries:
                    continue
                pending = self._pending.get(index)
                if pending is not None:
                    if final or now >= pending["due"]:
                        late = (max(0.0, pending["due"] - now)
                                if final and pending["due"] > now else None)
                        self._capture(index, now, pending["trigger"], late)
                    continue
                trigger = None
                if item.get("_check") is not None:
                    if item["_check"].search(self._text):
                        trigger = f"after_check={item['after_check']}"
                elif item.get("_marker") is not None:
                    if item["_marker"].search(self._text):
                        trigger = f"after_marker={item['after_marker']}"
                elif elapsed >= float(item["after_seconds"]):
                    trigger = f"after_seconds={item['after_seconds']}"
                if trigger is None:
                    continue
                due = now + float(item.get("delay", 0))
                if final or due <= now:
                    late = (max(0.0, due - now)
                            if final and due > now else None)
                    self._capture(index, now, trigger, late)
                else:
                    self._pending[index] = {"due": due, "trigger": trigger}
            if final:
                for index, item in enumerate(self.checkpoints):
                    if index in self._entries or index in self._pending:
                        continue
                    key = next(key for key in TRIGGERS if key in item)
                    self._entries[index] = {
                        "name": item["name"],
                        "trigger": f"{key}={item[key]}",
                        "required": bool(item.get("required", False)),
                        "nonblank": bool(item.get("nonblank", False)),
                        "missed": True,
                    }

    def run(self) -> None:
        while not self.stop_event.is_set():
            eof = self._drain()
            self._scan(time.monotonic())
            if eof:
                break
            self.stop_event.wait(self.poll)

    def finalize(self) -> list[dict]:
        """Stop watching and capture any final or pending checkpoints."""
        self.stop_event.set()
        self.join(5)
        self._drain()
        self._scan(time.monotonic(), final=True)
        return self.entries()

    def entries(self) -> list[dict]:
        with self._lock:
            return [self._entries[index] for index in sorted(self._entries)]
