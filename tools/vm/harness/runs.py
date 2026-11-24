# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Run directory discovery, extraction and safe pruning."""
from __future__ import annotations

import json
import shutil
import stat
import tempfile
from pathlib import Path

from .result_schema import schema_state, validate_result

# Linux sun_path is 108 bytes including the terminating NUL.
AF_UNIX_PATH_LIMIT = 107


def _result(path: Path) -> dict:
    try:
        data = json.loads((path / "result.json").read_text())
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def collect_runs(vm_dir: Path) -> list[dict]:
    runs = []
    root = Path(vm_dir) / "runs"
    try:
        entries = [entry for entry in root.iterdir() if entry.is_dir()]
    except OSError:
        return runs
    for path in entries:
        result_file = path / "result.json"
        result = _result(path) if result_file.is_file() else None
        data = result or {}
        problems = (validate_result(result) if result_file.is_file()
                    else ["missing result.json"])
        scenario = data.get("scenario") or {}
        runs.append({
            "run": path.name,
            "path": str(path),
            "mtime": path.stat().st_mtime if path.exists() else 0,
            "label": data.get("label") or "",
            "mode": data.get("mode") or "vendor",
            "scenario": scenario.get("name") if isinstance(scenario, dict) else None,
            "verdict": data.get("verdict") or "UNKNOWN",
            "rc": data.get("rc"),
            "known_failure": bool(data.get("known_failure")),
            "keep_overlay": bool(data.get("keep_overlay")),
            "overlays_retained": bool(data.get("overlays_retained")),
            "screenshot": (data.get("screenshot") or {}),
            "schema_state": schema_state(result, problems),
            "schema_ok": not problems,
            "schema_problem_count": len(problems),
            "schema_problems": problems[:8],
            "result": str(result_file) if result_file.is_file() else None,
        })
    runs.sort(key=lambda item: item["mtime"], reverse=True)
    return runs


def filter_runs(runs: list[dict], label: str | None = None,
                scenario: str | None = None, verdict: str | None = None) -> list[dict]:
    selected = runs
    if label:
        selected = [item for item in selected if label in item["label"]]
    if scenario:
        selected = [item for item in selected if item["scenario"] == scenario]
    if verdict:
        selected = [item for item in selected if item["verdict"] == verdict]
    return selected


def extract(vm_dir: Path, run: str, member: str, destination: Path) -> dict:
    root = (Path(vm_dir) / "runs").resolve()
    run_dir = (root / run).resolve()
    if root not in run_dir.parents:
        raise ValueError(f"run escapes runs directory: {run}")
    source = (run_dir / member).resolve()
    if source != run_dir and run_dir not in source.parents:
        raise ValueError(f"member escapes run directory: {member}")
    if not source.is_file():
        raise ValueError(f"not a regular file: {member}")
    destination = Path(destination)
    if destination.is_dir():
        destination = destination / source.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return {"run": run, "member": member, "source": str(source),
            "destination": str(destination), "size": destination.stat().st_size}


def qmp_socket_from_run(run_dir: Path) -> Path:
    """Return a run's QMP socket, honoring a long-path redirect pointer."""
    pointer = Path(run_dir) / "qmp.path"
    try:
        value = pointer.read_text().strip()
    except OSError:
        value = ""
    return Path(value) if value else Path(run_dir) / "qmp.sock"


def allocate_qmp_socket(run_dir: Path) -> tuple[Path, Path | None]:
    """Choose a QMP path that fits Linux's 108-byte AF_UNIX limit.

    Normal repositories keep the socket beside the run.  Deep worktree paths
    would exceed the limit, so redirect to a private temporary directory and
    leave a ``qmp.path`` pointer in the run directory.
    """
    run_dir = Path(run_dir)
    candidate = run_dir / "qmp.sock"
    if len(str(candidate).encode()) <= AF_UNIX_PATH_LIMIT:
        return candidate, None
    directory = Path(tempfile.mkdtemp(prefix="vm-qmp-",
                                      dir=tempfile.gettempdir()))
    socket_path = directory / "qmp.sock"
    try:
        (run_dir / "qmp.path").write_text(str(socket_path) + "\n")
    except OSError:
        shutil.rmtree(directory, ignore_errors=True)
        raise
    return socket_path, directory


def run_qmp_socket(vm_dir: Path, run: str) -> Path:
    """Resolve a run id/path to its live QMP socket without escaping runs/."""
    root = (Path(vm_dir) / "runs").resolve()
    candidate = Path(run)
    if candidate.is_absolute():
        run_dir = candidate.resolve()
    elif len(candidate.parts) > 1:
        run_dir = (Path(vm_dir) / candidate).resolve()
    else:
        run_dir = (root / candidate).resolve()
    if run_dir != root and root not in run_dir.parents:
        raise ValueError(f"run escapes runs directory: {run}")
    socket_path = qmp_socket_from_run(run_dir)
    if not socket_path.exists():
        raise ValueError(
            f"QMP socket missing: {socket_path}; start the run with "
            "--qmp-screenshot or use a persistent daemon")
    return socket_path


def latest_live(vm_dir: Path) -> dict:
    """Return the newest live QMP target (daemon first, then one-shot runs)."""
    root = Path(vm_dir) / "runs"
    daemon_dir = root / "daemon"
    try:
        status = json.loads((daemon_dir / "status.json").read_text())
    except (OSError, json.JSONDecodeError):
        status = {}
    daemon_qmp = daemon_dir / "qmp.sock"
    if (isinstance(status, dict) and status.get("state") == "ready"
            and daemon_qmp.exists()):
        return {"kind": "daemon", "directory": str(daemon_dir),
                "qmp": str(daemon_qmp)}

    candidates = []
    try:
        entries = list(root.iterdir())
    except OSError:
        entries = []
    for path in entries:
        if path.name == "daemon" or not path.is_dir():
            continue
        socket_path = qmp_socket_from_run(path)
        if not socket_path.exists() or (path / "result.json").exists():
            continue
        try:
            mode = socket_path.stat().st_mode
        except OSError:
            continue
        if not stat.S_ISSOCK(mode):
            continue
        candidates.append((socket_path.stat().st_mtime, path, socket_path))
    if not candidates:
        raise ValueError("no live VM found; start a run with --qmp-screenshot "
                         "or a persistent daemon")
    _, path, socket_path = max(candidates)
    return {"kind": "qmp", "directory": str(path), "qmp": str(socket_path)}


def prune(vm_dir: Path, keep: int = 20, apply: bool = False,
          label: str | None = None, scenario: str | None = None,
          allow_no_result: bool = False) -> dict:
    if keep < 0:
        raise ValueError("keep must be >= 0")
    runs = filter_runs(collect_runs(vm_dir), label=label, scenario=scenario)
    candidates: list[dict] = []
    protected: list[dict] = []
    for index, item in enumerate(runs):
        if index < keep:
            protected.append({**item, "reason": "keep-newest"})
        elif item["run"] == "daemon":
            # A live daemon owns overlays/sockets here; never prune it.
            protected.append({**item, "reason": "daemon"})
        elif item["result"] is None and not allow_no_result:
            # No result.json: not a completed managed run (daemon dirs,
            # regression bundles, interrupted runs). Explicit opt-in only.
            protected.append({**item, "reason": "no-result"})
        elif item["keep_overlay"] or item["overlays_retained"]:
            protected.append({**item, "reason": "retained-overlays"})
        else:
            candidates.append(item)
    freed = 0
    for item in candidates:
        path = Path(item["path"])
        if path.is_symlink():
            protected.append({**item, "reason": "symlink"})
            continue
        try:
            freed += sum(file.stat().st_size for file in path.rglob("*")
                         if file.is_file() and not file.is_symlink())
        except OSError:
            pass
    deleted = []
    if apply:
        for item in candidates:
            path = Path(item["path"])
            root = (Path(vm_dir) / "runs").resolve()
            if path.is_symlink() or root not in path.resolve().parents:
                protected.append({**item, "reason": "unsafe-path"})
                continue
            shutil.rmtree(path)
            deleted.append(item["run"])
    return {
        "keep": keep,
        "apply": apply,
        "allow_no_result": allow_no_result,
        "candidates": [item["run"] for item in candidates],
        "deleted": deleted,
        "protected": protected,
        "estimated_bytes": freed,
    }
