"""blinky firmware build service (docs/DEV_LOOP.md, "Build service").

Two processes share one job volume:
  api     HTTP + every git operation; never runs repository code (blinky_build.api)
  worker  no network; runs the firmware's own build and package steps (blinky_build.worker)
"""

SHA_RE = r"^[0-9a-f]{40}$"
