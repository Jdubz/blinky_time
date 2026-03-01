#!/usr/bin/env python3
"""
Bar-Pointer HMM Beat Tracking Prototype (Phase 3.1)

Joint tempo-phase HMM based on madmom's DBN beat tracker (Bock et al. 2016).
State space: (position_within_beat, beat_period) — tempo and phase are structurally coupled.

Key differences from current firmware (Bayesian tempo + CBSS):
- Phase advances deterministically each frame (no predict-and-countdown)
- Tempo changes ONLY at beat boundaries (position wraps to 0)
- Frame-level beat probability (beat fires when position wraps)
- Implicit tempo smoothing through sparse transition matrix

Evaluates against ground truth beat annotations on 18-track EDM test set.
"""

import json
import sys
import os
import numpy as np
import librosa
import soundfile as sf
from pathlib import Path


# --- Constants matching firmware ---
SAMPLE_RATE = 16000       # Firmware PDM mic rate
HOP_SIZE = 256            # ~16ms frames at 16kHz (firmware FFT-256 hop)
FRAME_RATE = SAMPLE_RATE / HOP_SIZE  # ~62.5 Hz

# Tempo range (matching firmware 60-200 BPM, 20 bins)
MIN_BPM = 60.0
MAX_BPM = 200.0
NUM_TEMPO_BINS = 20

# HMM parameters
LAMBDA_TRANSITION = 100    # madmom default: controls tempo change probability
BEAT_THRESHOLD = 0.0       # Min observation to fire beat (0 = always fire at position wrap)


def bpm_to_period_frames(bpm):
    """Convert BPM to beat period in frames."""
    return 60.0 * FRAME_RATE / bpm


def compute_odf(audio, sr):
    """
    Compute onset detection function similar to firmware BandFlux.
    Uses librosa's spectral flux with log compression and band weighting.
    """
    # Resample to firmware rate if needed
    if sr != SAMPLE_RATE:
        audio = librosa.resample(audio, orig_sr=sr, target_sr=SAMPLE_RATE)
        sr = SAMPLE_RATE

    # STFT with firmware-matching parameters
    n_fft = 512  # Slightly larger than firmware's 256 for better frequency resolution
    hop = HOP_SIZE
    S = np.abs(librosa.stft(audio, n_fft=n_fft, hop_length=hop))

    # Log compression (firmware: log(1 + gamma * mag), gamma=20)
    gamma = 20.0
    S_log = np.log1p(gamma * S)

    # Band weighting (firmware: bass 2.0x, mid 1.5x, high 0.1x)
    n_bins = S_log.shape[0]
    freq_bins = librosa.fft_frequencies(sr=sr, n_fft=n_fft)
    weights = np.ones(n_bins)
    for i, f in enumerate(freq_bins):
        if f < 250:      # Bass
            weights[i] = 2.0
        elif f < 2000:   # Mid
            weights[i] = 1.5
        else:            # High
            weights[i] = 0.1
    weights = weights[:, np.newaxis]
    S_weighted = S_log * weights

    # Spectral flux (half-wave rectified difference)
    flux = np.zeros(S_weighted.shape[1])
    for t in range(1, S_weighted.shape[1]):
        diff = S_weighted[:, t] - S_weighted[:, t-1]
        flux[t] = np.sum(np.maximum(0, diff))

    # Normalize
    if flux.max() > 0:
        flux = flux / flux.max()

    return flux


def build_bar_pointer_hmm(num_tempo_bins=NUM_TEMPO_BINS, min_bpm=MIN_BPM, max_bpm=MAX_BPM,
                          lambda_transition=LAMBDA_TRANSITION):
    """
    Build the bar-pointer HMM state space and transition model.

    State: (position, tempo_bin) where:
    - position: 0..period_frames[tempo_bin]-1 (varies per tempo)
    - tempo_bin: 0..num_tempo_bins-1

    Returns state definitions and sparse transition info.
    """
    # Compute beat periods for each tempo bin
    bpms = np.linspace(min_bpm, max_bpm, num_tempo_bins)
    periods = np.round(60.0 * FRAME_RATE / bpms).astype(int)

    # Build state list: [(tempo_bin, position), ...]
    states = []
    state_map = {}  # (tempo_bin, position) -> state_index
    beat_states = []  # indices of states where position == 0 (beat fires)

    for t in range(num_tempo_bins):
        for p in range(periods[t]):
            idx = len(states)
            states.append((t, p))
            state_map[(t, p)] = idx
            if p == 0:
                beat_states.append(idx)

    num_states = len(states)
    print(f"  HMM: {num_states} states ({num_tempo_bins} tempos × variable positions)")
    print(f"  Tempo range: {bpms[0]:.1f}-{bpms[-1]:.1f} BPM")
    print(f"  Period range: {periods[-1]}-{periods[0]} frames")
    print(f"  Beat states: {len(beat_states)}")

    # Build sparse transition matrix
    # For non-beat states (position > 0): deterministic advance position += 1
    # For beat states (position == 0): can transition to any tempo's position 1
    #   with probability proportional to exp(-lambda * |tempo_change|)

    # Pre-compute tempo transition probabilities (only used at beat states)
    tempo_trans = np.zeros((num_tempo_bins, num_tempo_bins))
    for i in range(num_tempo_bins):
        for j in range(num_tempo_bins):
            # Probability of transitioning from tempo i to tempo j
            # Based on ratio of periods (madmom uses log-period distance)
            ratio = periods[i] / periods[j]
            log_ratio = np.abs(np.log2(ratio))
            tempo_trans[i, j] = np.exp(-lambda_transition * log_ratio)
        # Normalize
        tempo_trans[i] /= tempo_trans[i].sum()

    return {
        'states': states,
        'state_map': state_map,
        'beat_states': set(beat_states),
        'num_states': num_states,
        'num_tempo_bins': num_tempo_bins,
        'periods': periods,
        'bpms': bpms,
        'tempo_trans': tempo_trans,
    }


def viterbi_online(odf, hmm):
    """
    Online Viterbi-style forward pass through the bar-pointer HMM.

    For each frame:
    - Non-beat states: position advances deterministically
    - Beat states (position wraps to 0): tempo can change according to tempo_trans

    Observation model: ODF value at beat states, uniform elsewhere.

    Returns beat times (in frames) and estimated BPM over time.
    """
    num_states = hmm['num_states']
    states = hmm['states']
    state_map = hmm['state_map']
    beat_states = hmm['beat_states']
    periods = hmm['periods']
    tempo_trans = hmm['tempo_trans']
    num_tempo_bins = hmm['num_tempo_bins']
    num_frames = len(odf)

    # Forward variable (log domain for numerical stability)
    log_alpha = np.full(num_states, -np.inf)

    # Initialize: uniform over all states
    log_alpha[:] = -np.log(num_states)

    beat_times = []
    bpm_trajectory = []

    for t in range(num_frames):
        new_log_alpha = np.full(num_states, -np.inf)

        # --- Transition ---
        for s in range(num_states):
            tempo_bin, position = states[s]
            period = periods[tempo_bin]

            if position == period - 1:
                # Next frame wraps to position 0 (beat!) — can change tempo
                for new_tempo in range(num_tempo_bins):
                    new_state = state_map.get((new_tempo, 0))
                    if new_state is not None:
                        trans_prob = tempo_trans[tempo_bin, new_tempo]
                        val = log_alpha[s] + np.log(trans_prob + 1e-30)
                        if val > new_log_alpha[new_state]:
                            new_log_alpha[new_state] = val
            else:
                # Deterministic advance: position += 1
                new_state = state_map.get((tempo_bin, position + 1))
                if new_state is not None:
                    new_log_alpha[new_state] = log_alpha[s]

        # --- Observation ---
        obs = odf[t]
        for s in range(num_states):
            tempo_bin, position = states[s]
            if position == 0:
                # Beat state: observation is ODF value (higher ODF = more likely beat)
                new_log_alpha[s] += np.log(obs + 1e-6)
            else:
                # Non-beat state: observation is (1 - ODF) to penalize beats at wrong positions
                new_log_alpha[s] += np.log(1.0 - obs * 0.9 + 1e-6)

        # Normalize (prevent underflow)
        max_val = np.max(new_log_alpha)
        if max_val > -np.inf:
            new_log_alpha -= max_val

        log_alpha = new_log_alpha

        # --- Beat detection: check if beat state has highest probability ---
        best_state = np.argmax(log_alpha)
        best_tempo, best_pos = states[best_state]
        bpm_trajectory.append(hmm['bpms'][best_tempo])

        if best_pos == 0:
            beat_times.append(t)

    return beat_times, bpm_trajectory


def viterbi_online_optimized(odf, hmm):
    """
    Optimized online forward pass — only tracks per-tempo-bin max probability.

    Key insight: within each tempo bin, position advances deterministically.
    We only need to track the probability for each (tempo_bin, position) pair,
    and the transition only happens at beat boundaries.

    This reduces from O(num_states) to O(num_tempo_bins * max_period) per frame,
    which is the same but with much better cache behavior.
    """
    num_tempo_bins = hmm['num_tempo_bins']
    periods = hmm['periods']
    tempo_trans = hmm['tempo_trans']
    bpms = hmm['bpms']
    num_frames = len(odf)

    # Per-tempo probability vectors
    # prob[tempo_bin][position] = log probability
    prob = []
    for t_bin in range(num_tempo_bins):
        p = np.full(periods[t_bin], -np.log(num_tempo_bins * periods[t_bin]))
        prob.append(p)

    beat_times = []
    bpm_trajectory = []
    last_beat_frame = -999

    for frame in range(num_frames):
        obs = odf[frame]
        new_prob = []

        # Collect beat-state probabilities from all tempos (position == period-1 → wraps to 0)
        beat_probs = np.full(num_tempo_bins, -np.inf)
        for t_bin in range(num_tempo_bins):
            period = periods[t_bin]
            beat_probs[t_bin] = prob[t_bin][period - 1]  # about to wrap

        # Compute new beat-state (position 0) probability for each tempo
        new_beat_prob = np.full(num_tempo_bins, -np.inf)
        for new_tempo in range(num_tempo_bins):
            for old_tempo in range(num_tempo_bins):
                trans = np.log(tempo_trans[old_tempo, new_tempo] + 1e-30)
                val = beat_probs[old_tempo] + trans
                if val > new_beat_prob[new_tempo]:
                    new_beat_prob[new_tempo] = val

        for t_bin in range(num_tempo_bins):
            period = periods[t_bin]
            new_p = np.full(period, -np.inf)

            # Position 0 (beat state): from tempo transition
            new_p[0] = new_beat_prob[t_bin] + np.log(obs + 1e-6)

            # Positions 1..period-1: deterministic shift from position-1
            for pos in range(1, period):
                new_p[pos] = prob[t_bin][pos - 1] + np.log(1.0 - obs * 0.9 + 1e-6)

            new_prob.append(new_p)

        # Normalize across all states
        all_vals = np.concatenate(new_prob)
        max_val = np.max(all_vals)
        if max_val > -np.inf:
            for t_bin in range(num_tempo_bins):
                new_prob[t_bin] -= max_val

        prob = new_prob

        # Find best state
        best_tempo = 0
        best_pos = 0
        best_val = -np.inf
        for t_bin in range(num_tempo_bins):
            local_best = np.argmax(prob[t_bin])
            if prob[t_bin][local_best] > best_val:
                best_val = prob[t_bin][local_best]
                best_tempo = t_bin
                best_pos = local_best

        bpm_trajectory.append(bpms[best_tempo])

        # Beat detection: position 0 is the beat state
        if best_pos == 0 and (frame - last_beat_frame) > periods[best_tempo] * 0.5:
            beat_times.append(frame)
            last_beat_frame = frame

    return beat_times, bpm_trajectory


def evaluate_beats(est_beats_sec, ref_beats_sec, tolerance_ms=200):
    """
    Evaluate beat tracking using F-measure with tolerance window.
    Matches firmware scoring (BEAT_TIMING_TOLERANCE_MS=200ms).
    """
    tolerance = tolerance_ms / 1000.0

    if len(est_beats_sec) == 0 or len(ref_beats_sec) == 0:
        return {'f1': 0.0, 'precision': 0.0, 'recall': 0.0,
                'est_count': len(est_beats_sec), 'ref_count': len(ref_beats_sec)}

    # Greedy matching
    ref_matched = set()
    est_matched = set()

    # For each estimated beat, find closest unmatched reference
    for i, est in enumerate(est_beats_sec):
        best_dist = float('inf')
        best_ref = -1
        for j, ref in enumerate(ref_beats_sec):
            if j in ref_matched:
                continue
            dist = abs(est - ref)
            if dist < best_dist and dist <= tolerance:
                best_dist = dist
                best_ref = j
        if best_ref >= 0:
            est_matched.add(i)
            ref_matched.add(best_ref)

    tp = len(ref_matched)
    precision = tp / len(est_beats_sec) if est_beats_sec else 0
    recall = tp / len(ref_beats_sec) if ref_beats_sec else 0
    f1 = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0

    return {'f1': f1, 'precision': precision, 'recall': recall,
            'est_count': len(est_beats_sec), 'ref_count': len(ref_beats_sec)}


def load_ground_truth(json_path, max_duration_sec=None):
    """Load beat times from ground truth JSON (firmware format)."""
    with open(json_path) as f:
        data = json.load(f)

    beats = []
    for hit in data.get('hits', []):
        t = hit['time']
        if hit.get('instrument') == 'beat' and hit.get('expectTrigger', True):
            if max_duration_sec is None or t <= max_duration_sec:
                beats.append(t)

    return sorted(beats), data.get('bpm', 0)


def process_track(audio_path, gt_path, max_duration_sec=30, hmm=None):
    """Process a single track: compute ODF, run HMM, evaluate."""
    # Load audio
    audio, sr = sf.read(audio_path)
    if audio.ndim > 1:
        audio = audio.mean(axis=1)  # Mono

    # Trim to max duration
    max_samples = int(max_duration_sec * sr)
    if len(audio) > max_samples:
        audio = audio[:max_samples]

    # Compute ODF
    odf = compute_odf(audio, sr)

    # Run HMM
    beat_frames, bpm_traj = viterbi_online_optimized(odf, hmm)

    # Convert beat frames to seconds
    est_beats_sec = np.array(beat_frames) * HOP_SIZE / SAMPLE_RATE

    # Load ground truth
    ref_beats_sec, expected_bpm = load_ground_truth(gt_path, max_duration_sec)

    # Evaluate
    result = evaluate_beats(est_beats_sec, ref_beats_sec)

    # BPM accuracy
    if len(bpm_traj) > 0 and expected_bpm > 0:
        avg_bpm = np.mean(bpm_traj[len(bpm_traj)//4:])  # Skip first 25% for lock-in
        bpm_ratio = avg_bpm / expected_bpm
        # Allow octave equivalence
        bpm_acc = max(
            1.0 - abs(bpm_ratio - 1.0),
            1.0 - abs(bpm_ratio - 2.0),
            1.0 - abs(bpm_ratio - 0.5),
            0.0
        )
    else:
        avg_bpm = 0
        bpm_acc = 0

    result['avg_bpm'] = avg_bpm
    result['expected_bpm'] = expected_bpm
    result['bpm_accuracy'] = bpm_acc

    return result


def main():
    music_dir = Path('/home/blinkytime/blinky_time/blinky-test-player/music/edm')
    max_duration = 30  # Match firmware testing

    # Find all track pairs
    tracks = []
    for gt_file in sorted(music_dir.glob('*.beats.json')):
        name = gt_file.stem.replace('.beats', '')
        audio_file = music_dir / f'{name}.mp3'
        if audio_file.exists():
            tracks.append((name, str(audio_file), str(gt_file)))

    if not tracks:
        print("No tracks found!")
        return

    print(f"Found {len(tracks)} tracks")
    print()

    # Build HMM
    print("Building bar-pointer HMM...")
    hmm = build_bar_pointer_hmm()
    print()

    # Process all tracks
    results = {}
    for name, audio_path, gt_path in tracks:
        print(f"Processing: {name}...", end=' ', flush=True)
        try:
            result = process_track(audio_path, gt_path, max_duration, hmm)
            results[name] = result
            print(f"F1={result['f1']:.3f}  BPM={result['avg_bpm']:.1f}/{result['expected_bpm']:.0f}  "
                  f"acc={result['bpm_accuracy']:.3f}")
        except Exception as e:
            print(f"ERROR: {e}")
            results[name] = {'f1': 0, 'avg_bpm': 0, 'expected_bpm': 0, 'bpm_accuracy': 0}

    # Summary
    print()
    print("=" * 80)
    print(f"{'Track':<30} {'F1':>6} {'Prec':>6} {'Rec':>6} {'BPM':>7} {'Exp':>5} {'Acc':>6}")
    print("-" * 80)

    f1_sum = 0
    bpm_acc_sum = 0
    count = 0
    for name, r in results.items():
        print(f"{name:<30} {r['f1']:>6.3f} {r.get('precision',0):>6.3f} "
              f"{r.get('recall',0):>6.3f} {r['avg_bpm']:>7.1f} {r['expected_bpm']:>5.0f} "
              f"{r['bpm_accuracy']:>6.3f}")
        f1_sum += r['f1']
        bpm_acc_sum += r['bpm_accuracy']
        count += 1

    if count > 0:
        print("-" * 80)
        print(f"{'AVERAGE':<30} {f1_sum/count:>6.3f} {'':>6} {'':>6} {'':>7} {'':>5} "
              f"{bpm_acc_sum/count:>6.3f}")
        print()
        print(f"HMM Prototype avg Beat F1: {f1_sum/count:.3f}")
        print(f"Firmware v33 avg Beat F1:   0.282 (4-device avg)")
        print(f"Firmware v33 best-dev F1:   0.360")
        print(f"Firmware v32 avg Beat F1:   0.265")
        print(f"Target (+70% from v32):     0.450")


if __name__ == '__main__':
    main()
