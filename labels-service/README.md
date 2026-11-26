# labels-service

The drum-label reviewer. The owner and helpers fix the corpus's kick and snare labels for the 18 `edm/` tracks (blinky-corpus#4) together, from the page at `https://blinky-time.joshwentworth.com/labels/`.

- There are no users, names, drafts, locks or Save button.
- **Each track has one shared set of labels.** Every edit saves by itself and appears on everyone else's page within a few seconds.
- **The service publishes the labels to blinky-corpus by itself.** Corpus CI checks them for vandalism, and they merge automatically.
- The site serves the page, built from this directory. The service here is its API, deployed on the build host from `deploy/build-host/labels/`.

## How it works

- **`state.py`: the shared labels.** Each track has one state: hits with stable ids, reviewed ranges, and a revision number.
  - The page sends edits as operations as they happen: `add`, `move`, `type` and `del` on hit ids, and `review` on a time range. It debounces them by 1 s and sends the difference between what it shows and what the server has.
  - The server applies operations in order, one revision each.
  - Every page pulls the operations it hasn't seen yet every 3 s and merges them in.
  - Two people never overwrite each other: they change different hits, and on the same hit the later edit wins.
  - Undo only undoes your own edits: incoming edits are applied to the undo history too.
  - A track's state starts from its human file, or else from its best seed (`seeds.json` in the corpus, `tools/labels/seeds.json`).
- **Publishing (`hosted.py` `AutoPublisher`)** happens once nobody has edited for 3 minutes, or 15 minutes after the first unpublished edit while people keep editing.
  - Every track whose labels differ from the corpus's `main` goes into one commit on top of `main`, on a branch `labels/auto-<time>-<id>`.
  - The branch is pushed with the corpus **deploy key** (write on blinky-corpus only) over `ssh://git@ssh.github.com:443`, with GitHub's host keys pinned in `github_known_hosts`. The service holds no GitHub token.
  - blinky-corpus's workflow `labels-pr.yml` opens the PR and turns on auto-merge. Its required **Scoring tests** check runs the label checks on the same push.
  - While the PR is open, later publishes, and `main` moving, rebuild and force-push the same branch.
  - A PR that fails CI stays open for the owner, and editing goes on.
  - When the batch lands, the next publish starts a new branch.
  - Partial progress is published: the scorer only uses a human file once it's `complete`.
- **Anti-vandalism** (`labelcheck.py`, the same file as the corpus's `tools/labels/labelcheck.py`; copy it when it changes). Before publishing, a file must:
  - match the schema;
  - not delete more than half of `main`'s hits;
  - not leave a class empty while a reference has hits in it.

  A file that fails is held back (`publish_refused` in the logs). Corpus CI runs the same checks as the gate.
- **The undo is git history.** A track whose file changes on `main` from outside (an owner's fix or revert) takes `main`'s version, and open pages reload it.
- **Rate limits** apply per client address, on requests and edits. The address is only ever kept as a keyed hash, in the logs.
- **Version.** `/api/config` reports `LABELS_SERVICE_VERSION`, the git tree hash of `labels-service/` (the image's build arg). The site stamps the same hash into the page, and the page asks for a reload when the two differ.

## Run it locally

```sh
python labels-service/server.py --edm path/to/blinky-corpus/edm   # http://localhost:8766
```

Locally, every change is written straight to `edm/drums_human/<stem>.drums.json`. The shared state lives in `edm/drums_human/.state/`, which is gitignored in the corpus. `--import-doc FILE` makes a label file (or a draft from the old reviewer) a track's shared state.

## Hosted

`deploy/build-host/labels/` has the details. The page calls the API across origins, and the API allows only the site's origin (`--cors-origin`). In hosted mode the server keeps its own clone of the corpus (`--checkout`, refreshed every minute), writes nothing into it, and publishes as above.

## API

| | |
|---|---|
| `GET /api/config` | hosted?, seed sources, version, publishing (`auto`, `local` or `off`) |
| `GET /api/tracks` | every track: source, counts, reviewed share, published / unpublished |
| `GET /api/track?stem=` | a track: audio, references, its shared labels (`rev`, `onsets` with ids, `reviewed_ranges`) |
| `GET /api/ops?stem=&since=REV` | the operations after REV, or `{"reset": true}` (reload the track) |
| `POST /api/ops?stem=` | `{"tab", "audio_sha256", "duration", "ops": [...]}` → `{"rev", "published", "unpublished"}` |
| `GET /audio/<stem>.mp3` | the audio (range requests) |
| `GET /healthz` | |

## Tests

- `tests/test_state.py`: the shared labels and their operations.
- `tests/test_server.py`: HTTP, local mode, two pages on one track, refusals, CORS, rate limits, and migrating the old drafts.
- `tests/test_publisher.py`: batches, rolling, landing, a closed PR, `main` moving, outside changes, vandalism, and the pinned host keys. It pushes to a local bare repository and fakes the public PR lookup (`tests/fake_github.py`).
- `python labels-service/check_js.py`: the page's syntax, and no external resources.
- CI's **Labels service (container, end to end)** job:
  - builds the image and runs it in the production sandbox against a bare stand-in for GitHub;
  - `tests/e2e_hosted.py` checks that two pages see each other's edits, a batch publishes itself, and the merge is noticed;
  - `tests/browser_hosted.py` runs two Chromium pages on one track, from another origin.
