# labels-service

The drum-label reviewer. The owner and helpers fix the corpus's kick and snare labels for the 18 `edm/` tracks (blinky-corpus#4) together, from the page at `https://blinky-time.joshwentworth.com/labels/`.

- There are no users, names, drafts, locks or Save button.
- **Each track has one shared set of labels.** Every edit saves by itself and appears on everyone else's page within a few seconds.
- **The labels reach blinky-corpus by themselves.** The corpus's scheduled workflow pulls them, checks them for vandalism, and auto-merges them. The service holds no credential.
- The site serves the page, built from this directory. The service here is its API, deployed on the build host from `deploy/build-host/labels/`.

## How it works

- **`state.py`: the shared labels.** Each track has one state: hits with stable ids, reviewed ranges, and a revision number.
  - The page sends edits as operations as they happen: `add`, `move`, `type` and `del` on hit ids, and `review` on a time range. It debounces them by 1 s and sends the difference between what it shows and what the server has.
  - The server applies operations in order, one revision each.
  - Every page pulls the operations it hasn't seen yet every 3 s and merges them in.
  - Two people never overwrite each other: they change different hits, and on the same hit the later edit wins.
  - Undo only undoes your own edits: incoming edits are applied to the undo history too.
  - A track's state starts from its human file, or else from its best seed (`seeds.json` in the corpus, `tools/labels/seeds.json`).
- **Publishing: the corpus pulls; the service holds no credential.** `GET /api/export` lists every edited track whose labels differ from the corpus's `main`. It's `ready` once nobody has edited for 3 minutes, or 15 minutes after the first unpublished edit while people keep editing.
  - blinky-corpus's workflow `labels-sync.yml` runs on a schedule (every 5 minutes) or on demand, as the corpus's own trusted code. It treats the export as untrusted input and checks it with `main`'s label checks.
  - It commits only `edm/drums_human/` files, as one batch on a `labels/auto-*` branch made from `main`, then opens the PR and turns on auto-merge.
  - A compromised service can only offer bad label files, which is what the checks are for. Nobody but the owner can push code to the corpus.
  - The service notices its labels landing on `main` (published).
  - Partial progress is published: the scorer only uses a human file once it's `complete`.
- **Anti-vandalism** (`labelcheck.py`, the same file as the corpus's `tools/labels/labelcheck.py`; copy it when it changes). Before publishing, a file must:
  - match the schema;
  - not delete more than half of `main`'s hits;
  - not leave a class empty while a reference has hits in it.

  A file that fails is held back (`publish_refused` in the logs). The corpus workflow and CI run the same checks as the gate.
- **The undo is git history.** A track whose file changes on `main` from outside (an owner's fix or revert) takes `main`'s version, and open pages reload it.
- **Rate limits** apply per client address, on requests and edits. The address is only ever kept as a keyed hash, in the logs.
- **Suspect bars and expected hits (hints, never labels).** A track's suspect map, `edm/drums_suspect/<stem>.json` in the corpus (blinky-corpus `scripts/drum_suspect.py`), is read from the same corpus checkout as the seeds and the audio.
  - The page draws its high and medium bars as a red and amber strip above *reviewed* (and in the overview), with the reason on hover. Reviewed bars fade.
  - **Next suspect** (`J`) jumps to the next unreviewed one. The setting *Review order: suspect first* makes *Reviewed → next* do the same, falling back to in-order when none are left.
  - The map's expected hits (schema 2) are hollow markers. One click, or `A` on a region, adds them through the normal `add` op. Markers aren't state: they're never sent, saved or published.
  - The track list's *suspect left* is the share of the high and medium bars not yet reviewed, and it sorts tracks with work left.
  - A page and an API on different versions both work: without `suspect` the page shows no strip or markers, and an older page ignores the field.
- **Version.** `/api/config` reports `LABELS_SERVICE_VERSION`, the git tree hash of `labels-service/` (the image's build arg). The site stamps the same hash into the page, and the page asks for a reload when the two differ.

## Run it locally

```sh
python labels-service/server.py --edm path/to/blinky-corpus/edm   # http://localhost:8766
```

Locally, every change is written straight to `edm/drums_human/<stem>.drums.json`. The shared state lives in `edm/drums_human/.state/`, which is gitignored in the corpus. `--import-doc FILE` makes a label file (or a draft from the old reviewer) a track's shared state.

## Hosted

`deploy/build-host/labels/` has the details. The page calls the API across origins, and the API allows only the site's origin (`--cors-origin`). In hosted mode the server keeps its own clone of the corpus (`--checkout`, refreshed every minute). It writes nothing into it and writes nothing anywhere else outside its state volume.

## API

| | |
|---|---|
| `GET /api/config` | hosted?, seed sources, version, publishing (`pulled`, `local` or `off`) |
| `GET /api/tracks` | every track: source, counts, reviewed share, published / unpublished, `suspect_s` and `suspect_left` (null without a suspect map) |
| `GET /api/track?stem=` | a track: audio, references, its shared labels (`rev`, `onsets` with ids, `reviewed_ranges`), `suspect` (`{schema, bars: [high and medium bars with their evidence], expected: [{time, type, p, at}]}`, or null) |
| `GET /api/ops?stem=&since=REV` | the operations after REV, or `{"reset": true}` (reload the track) |
| `POST /api/ops?stem=` | `{"tab", "audio_sha256", "duration", "ops": [...]}` → `{"rev", "published", "unpublished"}` |
| `GET /api/export` | for the corpus's labels-sync workflow: `{"ready", "tracks": {path: file text}, "held": {path: problems}}` |
| `GET /audio/<stem>.mp3` | the audio (range requests) |
| `GET /healthz` | |

## Tests

- `tests/test_state.py`: the shared labels and their operations.
- `tests/test_server.py`: HTTP, local mode, two pages on one track, refusals, CORS, rate limits, and migrating the old drafts.
- `tests/test_publisher.py`: the export (readiness, vandalism held back), landing, an older version landing, outside changes winning, a revert sticking, and no credential anywhere. A local bare repository stands in for the corpus.
- `python labels-service/check_js.py`: the page's syntax, and no external resources.
- CI's **Labels service (container, end to end)** job:
  - builds the image and runs it in the production sandbox against a bare stand-in for the corpus;
  - `tests/e2e_hosted.py` checks that two pages see each other's edits, the export offers the result, and merging it (as `labels-sync.yml` does) shows up as published;
  - `tests/browser_hosted.py` runs two Chromium pages on one track, from another origin.
