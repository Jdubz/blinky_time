"""server.py over HTTP: the track list and seeding, shared edits from several pages, local mode's
file writes, and the refusals (traversal, size, cross-site, rate limits, other audio)."""

from __future__ import annotations

import http.client
import io
import json
import threading

import pytest
from conftest import DUR, KICKS, sha, write_json

import hosted
import labelcheck
import server as srv

SITE = "https://site.example.invalid"


class Web:
    def __init__(self, app, port):
        self.app, self.port = app, port

    def req(self, method, path, body=None, headers=None, raw=None):
        h = {"Host": "127.0.0.1"}
        data = raw
        if body is not None:
            data = json.dumps(body).encode()
            h["Content-Type"] = "application/json"
        if data is not None:
            h["Content-Length"] = str(len(data))
        h.update(headers or {})
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        conn.request(method, path, body=data, headers=h)
        r = conn.getresponse()
        out = r.read()
        hd = dict(r.getheaders())
        conn.close()
        return r.status, hd, (json.loads(out) if hd.get("Content-Type", "").startswith("application/json") else out)

    def ops(self, stem, ops, tab="tab-aaaa", **kw):
        t = self.req("GET", f"/api/track?stem={stem}")[2]
        body = {"tab": tab, "audio_sha256": t["audio_sha256"], "duration": DUR, "ops": ops, **kw}
        return self.req("POST", f"/api/ops?stem={stem}", body)


def serve(app, **kw):
    s = srv.make_server(app, "127.0.0.1", 0, **kw)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return s, Web(app, s.server_address[1])


@pytest.fixture
def web(edm):
    s, w = serve(srv.local_app(edm))
    yield w
    s.shutdown()
    s.server_close()


@pytest.fixture
def hosted_web(edm, tmp_path):
    log = io.StringIO()
    app = srv.App(srv.Corpus(edm), tmp_path / "state", hosted_mode=True, log=hosted.JsonLog(log),
                  limits={**srv.DEFAULT_LIMITS, "ops": (3, 3600.0)})
    s, w = serve(app, allow_hosts=("api.example.invalid",), cors_origins=(SITE,))
    w.log = log
    yield w
    s.shutdown()
    s.server_close()


def test_track_list_and_seeding(web, edm):
    write_json(edm / "drums_human" / "c.drums.json", {
        "schema": 1, "track": "c", "audio_sha256": sha(edm / "c.mp3"), "duration": DUR,
        "started_from": {"source": "drums_adtof"}, "reviewed_ranges": [[0.0, 5.0]], "complete": False,
        "onsets": [{"time": 4.0, "type": "kick"}], "updated": "2026-11-24T00:00:00+00:00"})
    st, _, b = web.req("GET", "/api/tracks")
    tracks = {t["stem"]: t for t in b["tracks"]}
    assert st == 200 and tracks["a"]["label_source"] == "drums_adtof" and tracks["c"]["label_source"] == "human"
    assert tracks["a"]["disagreement_between"] == ["drums_adtof", "kick_weighted"]
    a = web.req("GET", "/api/track?stem=a")[2]
    assert a["seed"] == "drums_adtof" and a["rev"] == 0 and len(a["onsets"]) == 19
    assert all(o["id"].startswith("h") for o in a["onsets"]) and a["references"]["drums_adtof"]["kick"] == KICKS
    c = web.req("GET", "/api/track?stem=c")[2]
    assert c["seed"] == "human" and [o["time"] for o in c["onsets"]] == [4.0] and c["reviewed_ranges"] == [[0.0, 5.0]]


def test_local_mode_writes_each_change(web, edm):
    hp = edm / "drums_human" / "a.drums.json"
    web.req("GET", "/api/track?stem=a")
    assert not hp.exists()  # opening a track writes nothing
    st, _, b = web.ops("a", [{"op": "del", "id": "h1"}, {"op": "review", "a": 0, "b": 2, "on": True}])
    assert st == 200 and b["rev"] == 2 and b["unpublished"] is False and b["published"]
    doc = json.loads(hp.read_text())
    assert len(doc["onsets"]) == 18 and doc["reviewed_ranges"] == [[0.0, 2.0]] and doc["complete"] is False
    assert labelcheck.check_schema(doc, "a", sha(edm / "a.mp3"), DUR) == []


def test_two_pages_see_each_others_edits(web):
    t = web.req("GET", "/api/track?stem=a")[2]
    rev0 = t["rev"]
    web.ops("a", [{"op": "move", "id": "h1", "time": 0.51}], tab="tab-aaaa")
    web.ops("a", [{"op": "add", "id": "tab-bbbb-1", "time": 9.9, "type": "snare"}], tab="tab-bbbb")
    pulled = web.req("GET", f"/api/ops?stem=a&since={rev0}")[2]
    assert [(by, op["op"]) for _, by, op in pulled["ops"]] == [("tab-aaaa", "move"), ("tab-bbbb", "add")]
    v = web.req("GET", "/api/track?stem=a")[2]
    times = {o["id"]: o["time"] for o in v["onsets"]}
    assert times["h1"] == 0.51 and times["tab-bbbb-1"] == 9.9


@pytest.mark.parametrize("ops,status", [
    ([{"op": "add", "id": "x", "time": 99.0, "type": "kick"}], 400),
    ([{"op": "add", "id": "x", "time": 1.0, "type": "hihat"}], 400),
    ("nope", 400),
])
def test_bad_ops_are_refused(web, ops, status):
    assert web.ops("a", ops)[0] == status
    assert web.req("GET", "/api/track?stem=a")[2]["rev"] == 0


def test_other_audio_is_refused(web):
    st, _, b = web.ops("a", [{"op": "del", "id": "h1"}], audio_sha256="0" * 64)
    assert st == 409 and "audio" in b["error"]


@pytest.mark.parametrize("path", ["/api/track?stem=../a", "/api/track?stem=..%2Fedm%2Fa", "/api/track?stem=/etc/passwd",
                                  "/api/track?stem=a%00", "/audio/..%2Ftrack_manifest.mp3", "/api/track?stem=nope",
                                  "/api/track", "/api/ops?stem=a&since=x"])
def test_bad_paths(web, path):
    assert web.req("GET", path)[0] in (400, 404)


def test_post_refusals(web):
    data = json.dumps({"ops": []}).encode()
    assert web.req("POST", "/api/ops?stem=a", raw=data, headers={"Content-Type": "text/plain"})[0] == 415
    assert web.req("POST", "/api/ops?stem=a", raw=b"x", headers={"Content-Type": "application/json",
                                                                 "Content-Length": str(srv.MAX_POST_BYTES + 1)})[0] == 413
    assert web.req("POST", "/api/ops?stem=a", {"ops": []}, headers={"Origin": "https://evil.example"})[0] == 403
    assert web.req("GET", "/api/tracks", headers={"Host": "evil.example"})[0] == 403  # DNS rebinding
    assert web.req("POST", "/api/save?stem=a", {"x": 1})[0] == 404  # no saves any more


def test_audio_range(web, edm):
    raw = (edm / "a.mp3").read_bytes()
    st, h, b = web.req("GET", "/audio/a.mp3", headers={"Range": "bytes=10-19"})
    assert st == 206 and b == raw[10:20] and h["Content-Range"] == f"bytes 10-19/{len(raw)}"


def test_page_and_version(web):
    st, h, b = web.req("GET", "/")
    assert st == 200 and b"<title>Drum Reviewer</title>" in b and b'name="drum-reviewer-version"' in b
    assert web.req("GET", "/api/config")[2]["version"] == srv.VERSION


# --- hosted ---------------------------------------------------------------------------------


def test_hosted_cors_headers_logs(hosted_web):
    w = hosted_web
    pre = {"Origin": SITE, "Access-Control-Request-Method": "POST", "Access-Control-Request-Headers": "content-type"}
    st, h, _ = w.req("OPTIONS", "/api/ops?stem=a", headers=pre)
    assert st == 204 and h["Access-Control-Allow-Origin"] == SITE
    assert w.req("OPTIONS", "/api/ops?stem=a", headers={**pre, "Origin": "https://evil.example"})[0] == 403
    st, h, _ = w.req("GET", "/api/tracks", headers={"Origin": SITE})
    assert h["Access-Control-Allow-Origin"] == SITE and "frame-ancestors 'none'" in h["Content-Security-Policy"]
    assert w.ops("a", [{"op": "del", "id": "h1"}])[0] == 200
    evs = [json.loads(line) for line in w.log.getvalue().splitlines()]
    assert any(e["event"] == "edit" and e["ops"] == 1 for e in evs)
    assert all(len(e["ip"]) == 16 for e in evs if e["event"] == "http") and "127.0.0.1" not in w.log.getvalue()
    assert w.req("GET", "/api/config")[2]["publishing"] == "off"  # no clone to follow main in this test


def test_hosted_writes_nothing_into_the_clone(hosted_web, edm):
    hosted_web.ops("a", [{"op": "del", "id": "h1"}])
    assert not (edm / "drums_human").exists()
    assert hosted_web.req("GET", "/api/track?stem=a")[2]["unpublished"] is True


def test_edit_rate_limit(hosted_web):
    codes = [hosted_web.ops("a", [{"op": "move", "id": "h1", "time": 0.5 + i / 100}])[0] for i in range(5)]
    assert codes[:3] == [200, 200, 200] and codes[3] == 429


def test_migrating_the_old_drafts(edm, tmp_path):
    """The first hosted version kept a draft per person; the newest becomes the track's shared state."""
    st = tmp_path / "state"
    doc = {"schema": 1, "track": "a", "audio_sha256": sha(edm / "a.mp3"), "duration": DUR,
           "started_from": {"source": "drums_adtof"}, "reviewed_ranges": [], "complete": False,
           "onsets": [{"time": t, "type": "kick"} for t in KICKS[:5]], "updated": "2026-11-26T02:11:18+00:00",
           "draft": True, "contributor": "Owner"}
    write_json(st / "drafts" / "a" / "n-0123456789abcdef0123.json", doc)
    write_json(st / "drafts" / "a" / "c-older-client-id.json", {**doc, "onsets": [], "updated": "2026-01-01T00:00:00+00:00"})
    write_json(st / "locks.json", {"locks": []})
    write_json(st / "pending.json", {"pending": {}})
    app = srv.App(srv.Corpus(edm), st, hosted_mode=True, log=hosted.JsonLog(io.StringIO()))
    app.migrate_old_state()
    v = app.store.view("a")
    assert [o["time"] for o in v["onsets"]] == KICKS[:5] and v["edited"]
    assert not (st / "drafts").exists() and not (st / "locks.json").exists() and not (st / "pending.json").exists()
    app.migrate_old_state()  # idempotent
    assert app.store.view("a")["rev"] == 1


def test_import_doc_refuses_other_audio(edm, tmp_path):
    app = srv.local_app(edm)
    with pytest.raises(srv.ApiError):
        app.import_doc("a", {"audio_sha256": "0" * 64, "onsets": []})
    app.import_doc("b", {"audio_sha256": sha(edm / "b.mp3"), "onsets": [{"time": 2.0, "type": "snare"}],
                         "duration": DUR})
    # local mode: an import is written like any edit
    assert json.loads((edm / "drums_human" / "b.drums.json").read_text())["onsets"] == [
        {"time": 2.0, "type": "snare"}]
