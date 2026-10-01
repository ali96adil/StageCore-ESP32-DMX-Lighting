Import("env")

import os
import subprocess


def git(project_dir, *args):
    return subprocess.check_output(
        ["git", "-C", project_dir, *args],
        text=True,
        stderr=subprocess.STDOUT,
    ).strip()


project_dir = env.subst("$PROJECT_DIR")
expected = os.environ.get("EXPECTED_SHA", "").strip()
try:
    actual = git(project_dir, "rev-parse", "HEAD")
except Exception:
    actual = "unknown"

if expected and actual != expected:
    raise RuntimeError(
        "StageCore firmware source revision mismatch: "
        f"expected {expected}, got {actual}"
    )

revision = expected or actual
if not revision:
    revision = "unknown"

if not expected and revision != "unknown":
    try:
        dirty = git(project_dir, "status", "--porcelain", "--untracked-files=normal")
    except Exception:
        dirty = "unknown"
    if dirty:
        revision += "-dirty"

env.Append(
    CPPDEFINES=[
        ("STAGECORE_BUILD_REVISION", '\\"' + revision + '\\"'),
    ]
)

print("StageCore build revision:", revision)
