"""Heavy CI jobs of a firmware commit, run by the worker (docs/DEV_LOOP.md, section 1, "Later
(phase 3)").

A check is one job of the commit's OWN `.github/workflows/pr-validation.yml`, run step for step
as GitHub runs it, so the gate (its scripts, floors and flags) has one definition: the firmware
repo's. Only the jobs in CHECKS are taken. The worker has no network, so a step that would
download something is replaced by what the image provides, and only when the step consists of
nothing else:

  uses: actions/checkout             the job's checkout (the api wrote it, branch commits only)
  uses: actions/setup-python         the image's Python, if the step asks for its version
  uses: arduino/setup-arduino-cli    the image's arduino-cli, if the step asks for its version
  git submodule update --init [--depth N] corpus
                                     the corpus the commit pins, which the api fetched from the
                                     corpus repo's own branches (never by SHA) and put in the job
  pip install <packages>             every package named installed in the image (PROVIDED_PIP)
  arduino-cli config init / bash tools/setup_tflite_lib.sh [--ci]
                                     the toolchain baked into the image, the commit's TFLite pin
                                     included (the worker checks the commit's toolchain pin)

A step whose `env` names a compiler (CC, CXX) the image doesn't have is refused the same way
(the NN harness pins `CXX: g++-13`).

Everything else runs, in order, as `bash --noprofile --norc -eo pipefail` in the checkout, with
the job's and the step's `env`. A step the worker can't run as written (an expression, a step
`if`, another shell or action, a working-directory, a download step mixed with other commands)
makes the check fail as an infrastructure error naming the step: the image needs updating, the
gate never silently loses a step. A step that fails makes the check fail; the steps after it
are skipped, as on GitHub. A command that needs the network fails, loudly, like any other.
"""

import re
import shlex
import shutil
from dataclasses import dataclass, field

WORKFLOW = ".github/workflows/pr-validation.yml"

# The jobs a check may run: the heavy ones (PR Validation's pace), by their workflow job id.
CHECKS = ("tracker-harness", "tracker-harness-nn")

# Python packages the image installs (requirements-worker.txt), by normalized name.
PROVIDED_PIP = frozenset({"numpy", "imageio-ffmpeg", "pytest"})

_PROVIDED_LINES = (
    re.compile(r"git submodule update --init(?: --depth \d+)? corpus"),
    re.compile(r"arduino-cli config init"),
    re.compile(r"bash tools/setup_tflite_lib\.sh(?: --ci)?"),
)
_PIP = re.compile(r"(?:python3? -m )?pip3? install (.+)")
_REQ_NAME = re.compile(r"^([A-Za-z0-9][A-Za-z0-9._-]*)")
_STEP_KEYS = {"name", "run", "env", "uses", "with", "id", "timeout-minutes", "continue-on-error"}


class Unsupported(Exception):
    """The worker can't run this job as written (an infrastructure error, not a failure)."""


@dataclass
class Step:
    name: str
    provided: str | None = None  # why it is the image's, instead of run
    script: str | None = None
    env: dict = field(default_factory=dict)
    timeout_s: float | None = None


def _norm(pkg):
    return re.sub(r"[-_.]+", "-", pkg).lower()


def _provided_line(line, image):
    """Why ``line`` is provided by the image, or None if it must run."""
    for pat in _PROVIDED_LINES:
        if pat.fullmatch(line):
            return line
    m = _PIP.fullmatch(line)
    if m:
        pkgs = []
        for tok in shlex.split(m.group(1)):
            if tok.startswith("-"):
                return None  # an option (-r, -e, --index-url...): not ours to interpret
            name = _REQ_NAME.match(tok)
            if not name:
                return None
            pkgs.append(_norm(name.group(1)))
        if pkgs and all(p in image["pip"] for p in pkgs):
            return f"pip packages in the image: {', '.join(pkgs)}"
    return None


def _env(raw, where):
    if raw is None:
        return {}
    if not isinstance(raw, dict):
        raise Unsupported(f"{where}: env is not a mapping")
    out = {}
    for k, v in raw.items():
        v = "" if v is None else str(v).lower() if isinstance(v, bool) else str(v)
        if "${{" in v:
            raise Unsupported(f"{where}: env {k} uses an expression")
        out[str(k)] = v
    return out


def plan(workflow, job_id, image):
    """The steps of ``job_id`` as the worker runs them. ``image``: {"python": "3.11",
    "arduino_cli": "1.3.1", "pip": set of normalized names}. Raises Unsupported."""
    if job_id not in CHECKS:
        raise Unsupported(f"{job_id!r} is not a check this service runs ({', '.join(CHECKS)})")
    jobs = (workflow or {}).get("jobs") if isinstance(workflow, dict) else None
    job = (jobs or {}).get(job_id)
    if not isinstance(job, dict):
        raise Unsupported(f"{WORKFLOW} has no job {job_id!r} at this commit")
    if job.get("strategy") or job.get("container") or job.get("services"):
        raise Unsupported(f"{job_id}: a matrix, container or services job")
    defaults = ((job.get("defaults") or {}).get("run") or {}) if isinstance(job, dict) else {}
    if defaults:
        raise Unsupported(f"{job_id}: run defaults ({sorted(defaults)})")
    job_env = _env(job.get("env"), job_id)
    steps = []
    for i, raw in enumerate(job.get("steps") or [], 1):
        if not isinstance(raw, dict):
            raise Unsupported(f"{job_id} step {i}: not a mapping")
        name = str(raw.get("name") or raw.get("uses") or f"step {i}")
        where = f"{job_id} step {i} ({name})"
        extra = set(raw) - _STEP_KEYS
        if extra:
            raise Unsupported(f"{where}: {', '.join(sorted(extra))} not supported")
        if raw.get("continue-on-error"):
            raise Unsupported(f"{where}: continue-on-error not supported")
        uses = raw.get("uses")
        if uses is not None:
            why = _action(str(uses), raw.get("with") or {}, image, where)
            steps.append(Step(name, provided=why))
            continue
        script = raw.get("run")
        if not isinstance(script, str) or not script.strip():
            raise Unsupported(f"{where}: neither run nor uses")
        if "${{" in script:
            raise Unsupported(f"{where}: the script uses an expression")
        lines = [ln.strip() for ln in script.splitlines()]
        lines = [ln for ln in lines if ln and not ln.startswith("#")]
        why = [_provided_line(ln, image) for ln in lines]
        timeout = raw.get("timeout-minutes")
        timeout_s = float(timeout) * 60 if isinstance(timeout, (int, float)) else None
        if all(why):
            steps.append(Step(name, provided="; ".join(why)))
        elif any(why):
            raise Unsupported(f"{where}: a download step mixed with other commands")
        else:
            env = {**job_env, **_env(raw.get("env"), where)}
            for key in ("CC", "CXX"):
                tool = env.get(key)
                if tool and not _has_tool(image, tool):
                    raise Unsupported(f"{where}: {key}={tool}, which the image doesn't have")
            steps.append(Step(name, script=script, env=env, timeout_s=timeout_s))
    if not any(s.script for s in steps):
        raise Unsupported(f"{job_id}: no step left to run")
    return steps


def _has_tool(image, tool):
    """Whether the image has the compiler a step names (``image["tools"]`` in tests)."""
    if "tools" in image:
        return tool in image["tools"]
    return shutil.which(tool) is not None


def _action(uses, with_, image, where):
    action, _, _ref = uses.partition("@")
    if action == "actions/checkout":
        if with_:
            raise Unsupported(f"{where}: checkout options ({sorted(with_)})")
        return "the job's checkout of the commit"
    if action == "actions/setup-python":
        want = str(with_.get("python-version", ""))
        if want != image["python"]:
            raise Unsupported(f"{where}: Python {want!r}, the image has {image['python']}")
        return f"the image's Python {image['python']}"
    if action == "arduino/setup-arduino-cli":
        want = str(with_.get("version", ""))
        if want != image["arduino_cli"]:
            raise Unsupported(
                f"{where}: arduino-cli {want!r}, the image has {image['arduino_cli']}"
            )
        return f"the image's arduino-cli {image['arduino_cli']}"
    raise Unsupported(f"{where}: action {action} not supported")


def job_timeout_s(workflow, job_id, default):
    job = ((workflow or {}).get("jobs") or {}).get(job_id) or {}
    t = job.get("timeout-minutes")
    return float(t) * 60 if isinstance(t, (int, float)) else float(default)
