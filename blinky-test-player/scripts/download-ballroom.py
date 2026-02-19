#!/usr/bin/env python3
"""
download-ballroom.py - Download and extract the Ballroom dataset audio.

Downloads the Ballroom dataset (~1.4 GB) from MTG/UPF with resume support,
then extracts WAV files into the music/ballroom/ directory.

Usage:
  python download-ballroom.py              # Download + extract
  python download-ballroom.py --extract    # Extract only (if already downloaded)
  python download-ballroom.py --status     # Check download progress
"""

import argparse
import os
import sys
import tarfile
import time
import urllib.request

MUSIC_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'music')
TARBALL_PATH = os.path.join(MUSIC_DIR, 'data1.tar.gz')
EXTRACT_DIR = os.path.join(MUSIC_DIR, 'ballroom')
URL = 'https://mtg.upf.edu/ismir2004/contest/tempoContest/data1.tar.gz'
EXPECTED_SIZE = 1453888083  # bytes


def download():
    """Download with resume support."""
    os.makedirs(MUSIC_DIR, exist_ok=True)

    start_byte = 0
    if os.path.exists(TARBALL_PATH):
        start_byte = os.path.getsize(TARBALL_PATH)
        if start_byte >= EXPECTED_SIZE:
            print(f'Download already complete ({start_byte / 1024 / 1024:.0f} MB)')
            return True
        print(f'Resuming from {start_byte / 1024 / 1024:.0f} MB')

    req = urllib.request.Request(URL)
    if start_byte > 0:
        req.add_header('Range', f'bytes={start_byte}-')

    try:
        response = urllib.request.urlopen(req, timeout=30)
    except Exception as e:
        print(f'Connection failed: {e}', file=sys.stderr)
        return False

    total = EXPECTED_SIZE
    mode = 'ab' if start_byte > 0 else 'wb'

    print(f'Downloading Ballroom dataset ({total / 1024 / 1024:.0f} MB)...')
    start_time = time.time()

    with open(TARBALL_PATH, mode) as f:
        downloaded = start_byte
        last_report = time.time()
        try:
            while True:
                chunk = response.read(1024 * 1024)  # 1MB chunks
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                now = time.time()
                if now - last_report > 5:
                    pct = downloaded / total * 100
                    elapsed = now - start_time
                    speed = (downloaded - start_byte) / elapsed / 1024
                    remaining = (total - downloaded) / (speed * 1024) if speed > 0 else 0
                    print(f'  {downloaded / 1024 / 1024:.0f} / {total / 1024 / 1024:.0f} MB '
                          f'({pct:.1f}%) - {speed:.0f} KB/s - ~{remaining / 60:.0f} min remaining')
                    last_report = now
        except KeyboardInterrupt:
            print(f'\nDownload paused at {downloaded / 1024 / 1024:.0f} MB. Run again to resume.')
            return False

    elapsed = time.time() - start_time
    print(f'Download complete! ({downloaded / 1024 / 1024:.0f} MB in {elapsed / 60:.1f} min)')
    return True


def extract():
    """Extract WAV files from the tarball."""
    if not os.path.exists(TARBALL_PATH):
        print('Tarball not found. Run download first.', file=sys.stderr)
        return False

    size = os.path.getsize(TARBALL_PATH)
    if size < EXPECTED_SIZE * 0.99:
        print(f'Tarball appears incomplete ({size / 1024 / 1024:.0f} / {EXPECTED_SIZE / 1024 / 1024:.0f} MB)',
              file=sys.stderr)
        return False

    os.makedirs(EXTRACT_DIR, exist_ok=True)
    print(f'Extracting WAV files to {EXTRACT_DIR}...')

    count = 0
    with tarfile.open(TARBALL_PATH, 'r:gz') as tar:
        for member in tar.getmembers():
            if member.name.endswith('.wav'):
                # Flatten directory structure - extract just the filename
                member.name = os.path.basename(member.name)
                tar.extract(member, EXTRACT_DIR)
                count += 1
                if count % 100 == 0:
                    print(f'  Extracted {count} files...')

    print(f'Extracted {count} WAV files to {EXTRACT_DIR}')
    return True


def status():
    """Check download/extraction status."""
    if os.path.exists(TARBALL_PATH):
        size = os.path.getsize(TARBALL_PATH)
        pct = size / EXPECTED_SIZE * 100
        complete = 'COMPLETE' if size >= EXPECTED_SIZE * 0.99 else 'PARTIAL'
        print(f'Tarball: {size / 1024 / 1024:.0f} / {EXPECTED_SIZE / 1024 / 1024:.0f} MB ({pct:.1f}%) [{complete}]')
    else:
        print('Tarball: not downloaded')

    if os.path.exists(EXTRACT_DIR):
        wavs = [f for f in os.listdir(EXTRACT_DIR) if f.endswith('.wav')]
        print(f'Extracted: {len(wavs)} WAV files in {EXTRACT_DIR}')
    else:
        print('Extracted: not yet')

    gt_dir = os.path.join(MUSIC_DIR, 'ballroom-gt')
    if os.path.exists(gt_dir):
        gts = [f for f in os.listdir(gt_dir) if f.endswith('.json')]
        print(f'Ground truth: {len(gts)} annotation files in {gt_dir}')


def main():
    parser = argparse.ArgumentParser(description='Download and extract the Ballroom dataset')
    parser.add_argument('--extract', action='store_true', help='Extract only (skip download)')
    parser.add_argument('--status', action='store_true', help='Check download/extraction status')
    args = parser.parse_args()

    if args.status:
        status()
        return

    if args.extract:
        extract()
        return

    if download():
        extract()


if __name__ == '__main__':
    main()
