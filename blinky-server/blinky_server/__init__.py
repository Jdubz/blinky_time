# blinky_scoring (scoring and label loading) comes from the blinky-corpus submodule, corpus/
# (#357): the labels and the code that scores against them live together, so the firmware's
# corpus gate and this server score the same way. Set here, before any blinky_server module
# imports it; a missing submodule is an error.
import sys as _sys
from pathlib import Path as _Path

_CORPUS = _Path(__file__).resolve().parents[2] / "corpus"
if not (_CORPUS / "blinky_scoring" / "__init__.py").is_file():
    raise ImportError(f"{_CORPUS} has no blinky_scoring: run `git submodule update --init corpus`")
if str(_CORPUS) not in _sys.path:
    _sys.path.insert(0, str(_CORPUS))
