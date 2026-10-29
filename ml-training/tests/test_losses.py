"""The shift-tolerant loss (#290): label-time uncertainty in the loss, not in widened targets.

Needs torch, so it runs in the trainer-contract CI job.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

torch = pytest.importorskip("torch")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from train import beat_this_bce, shift_tolerant_bce  # noqa: E402

T = 40
LABEL = 20


def labels(*frames: int) -> torch.Tensor:
    y = torch.zeros(1, T, 1)
    for f in frames:
        y[0, f, 0] = 1.0
    return y


def spike(at: int, height: float = 0.9, floor: float = 0.01) -> torch.Tensor:
    p = torch.full((1, T, 1), floor)
    p[0, at, 0] = height
    return p


def loss(p: torch.Tensor, y: torch.Tensor, **kw) -> float:
    return float(beat_this_bce(p, y, pos_weight=10.0, **kw))


def test_firing_within_the_tolerance_costs_nothing_more_than_on_the_label() -> None:
    y = labels(LABEL)
    on = loss(spike(LABEL), y)
    for off in (-3, -1, 1, 3):
        assert loss(spike(LABEL + off), y) == pytest.approx(on, rel=1e-6)
    assert loss(spike(LABEL + 4), y) > on  # outside ±3 the label is missed


def test_old_shift_bce_still_penalises_a_prediction_one_frame_off() -> None:
    """Why the new loss exists: shift_bce pools positives but counts the off-label frame as a
    negative, so it prefers on-label even inside the tolerance."""
    y = labels(LABEL)
    old_on = float(shift_tolerant_bce(spike(LABEL), y, pos_weight=10.0))
    old_off = float(shift_tolerant_bce(spike(LABEL + 2), y, pos_weight=10.0))
    assert old_off > old_on


def test_a_second_peak_near_a_label_is_free_and_a_far_one_costs() -> None:
    y = labels(LABEL)
    base = spike(LABEL)
    near = base.clone()
    near[0, LABEL + 3, 0] = 0.9  # a second peak within the tolerance: not a false positive
    far = base.clone()
    far[0, LABEL + 12, 0] = 0.9  # far from any label: a false positive
    assert loss(near, y) == pytest.approx(loss(base, y), rel=1e-6)
    assert loss(far, y) > loss(base, y) + 0.01


def test_matches_the_formula_on_random_inputs() -> None:
    g = torch.Generator().manual_seed(0)
    p = torch.rand(3, T, 2, generator=g).clamp(1e-7, 1 - 1e-7)
    y = (torch.rand(3, T, 2, generator=g) > 0.9).float()
    pw = torch.tensor([5.0, 9.0])

    def m(x: torch.Tensor, half: int) -> torch.Tensor:
        out = torch.empty_like(x)
        for t in range(T):
            out[:, t] = x[:, max(0, t - half):t + half + 1].amax(dim=1)
        return out

    mp, my = m(p, 3), m(y, 6)
    want = -(y * pw * torch.log(mp) + (1 - my) * torch.log(1 - mp)).mean()
    got = beat_this_bce(p, y, pos_weight=pw, tolerance_frames=3, neg_ignore_frames=6)
    assert float(got) == pytest.approx(float(want), rel=1e-5)


def test_gradient_reaches_only_the_strongest_prediction_near_a_label() -> None:
    y = labels(LABEL)
    p = spike(LABEL + 1, height=0.6)
    p[0, LABEL, 0] = 0.4
    p.requires_grad_(True)
    beat_this_bce(p, y, pos_weight=10.0).backward()
    g = p.grad[0, :, 0]
    assert g[LABEL + 1] < 0  # pushed up
    assert g[LABEL] == 0  # not the max within ±3, and not a negative: no gradient
