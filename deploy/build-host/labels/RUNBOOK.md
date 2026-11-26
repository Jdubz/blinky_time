# The labels service on the build host

The labels service (`labels-service/`) runs on the build host as the compose project `blinky-labels`. Its page is `https://blinky-time.joshwentworth.com/labels/`, and its API is `https://blinky-labels-api.joshwentworth.com`.

- **The tunnel.** The API is reached only through the host's existing artifacts Cloudflare tunnel. Its connector joins the internal network `blinky-labels-edge` and has one ingress rule for the API hostname. Nothing is published on a host port.
- **The firewall.** The service's egress is fenced by `firewall.sh`: TCP 443 to public addresses (fetching the corpus from GitHub) and nothing private.
- **No credential.** The service writes nowhere outside its state volume. The corpus pulls its labels (`labels-sync.yml`, every 5 minutes, running the corpus's own trusted code).
- **Deploys.** The pull deployer (`deploy/build-host/deployer/`) runs `deploy.sh` for every gated master commit that changes `labels-service/` or this directory. No GitHub runner is involved.
- **Host values.** Every host-specific value lives in `labels.env` (gitignored; template `labels.env.example`).

## Install (once per host)

1. **A deploy clone** of blinky_time, used only by the deployer: `git clone https://github.com/Jdubz/blinky_time.git ~/blinky-build-host`.
2. **`labels.env`:** in that clone, copy `deploy/build-host/labels/labels.env.example` to `labels.env` and fill it in. The values are two free /24s inside one /23, their gateways and the service's addresses, the cpuset, and the tunnel connector's container name. When moving the existing stack, use the values it already runs with, so its networks and firewall stay identical.
3. **The firewall unit.** It runs `firewall.sh` before dockerd at every boot:
   ```
   cd ~/blinky-build-host/deploy/build-host/labels
   sed "s#^ExecStart=.*#ExecStart=$PWD/firewall.sh apply#" blinky-labels-firewall.service \
     | sudo tee /etc/systemd/system/blinky-labels-firewall.service >/dev/null
   sudo systemctl daemon-reload && sudo systemctl enable --now blinky-labels-firewall.service
   sudo ./firewall.sh check    # "blinky-labels firewall OK (20 rules)"
   ```
4. **No credential.** The service holds no key or token. blinky-corpus's scheduled workflow `labels-sync.yml` pulls the shared labels from the public `GET /api/export` and publishes them with its own `GITHUB_TOKEN`.
5. **The tunnel.**
   - The connector's stack (homelab-stacks `stacks/seaweedfs`, `cloudflared-artifacts`) lists `blinky-labels-edge` as an external network. `network.sh` creates it, and it must exist before that stack starts.
   - The connector's `config.yml` has the rule `blinky-labels-api.joshwentworth.com → http://blinky-labels:8766` above its 404 catch-all.
   - DNS: a proxied CNAME `blinky-labels-api` points at that tunnel.
6. **First deploy:** `deploy.sh`. Then run `verify.sh --public https://blinky-labels-api.joshwentworth.com`; every line should say `ok`.
7. **The deployer:**
   ```
   cd ~/blinky-build-host/deploy/build-host/deployer
   for u in blinky-build-host-deploy.service blinky-build-host-deploy.timer; do
     sed "s#@USER@#$USER#g; s#@HOME@#$HOME#g; s#@CLONE@#$HOME/blinky-build-host#g" $u | sudo tee /etc/systemd/system/$u >/dev/null
   done
   sudo systemctl daemon-reload && sudo systemctl enable --now blinky-build-host-deploy.timer
   python3 deployer.py --status
   ```
   - It needs `gh` logged in on the host, the same as the test box's deployer. That's host-side only: the container never sees a token.
   - Optionally, `~/.config/blinky-build-host/deployer.env` can set `LOKI_PUSH_URL` (the Alloy stack's Loki push endpoint), so the deployer's log also goes to Loki.
   - Record the commit you deployed by hand, so the next pass doesn't redeploy it: `python3 deployer.py --accept $(git rev-parse HEAD)`.

## Day to day

- **Deploys are automatic.** Merge to master and the next pass, within 2 minutes after CI and the review are green, rebuilds and restarts the service. A failed deploy rolls back to the previous commit.
  - The deployer's log: `journalctl -u blinky-build-host-deploy`.
  - The deployer's state: `python3 deployer.py --status`.
  - A refused commit: fix it on master, or `--accept SHA`.
- **What persists across deploys:**
  - the volume `blinky-labels-state`: the shared labels of every track (`tracks/<stem>.json`), what was published and the ip-hash key;
  - the volume `blinky-labels-corpus`: the clone, rebuilt from GitHub if lost;
  - the firewall unit and the edge network.
- **Back up the shared labels:** `docker run --rm -v blinky-labels-state:/s:ro -v "$PWD":/o busybox tar czf /o/labels-state.tgz -C /s .`. What's published is also in git.
- **Logs:**
  - The service writes one JSON line per request (`event="http"`) plus `edit`, `export`, `published`, `publish_refused`, `adopted_main`, `rate_limited`, `refresh_failed` and `error`. Query them with `{service="blinky-labels"}` in Loki.
  - The dashboard and alerts live in Josh-Wentworth/observability (Blinky — Labels).
- **After a tailscale, docker or kernel upgrade:** run `verify.sh`.

## Reacting

- **Vandalism that got through:**
  - Revert the merged commit on blinky-corpus `main` with an ordinary PR. The service takes `main`'s version of the track (`adopted_main`).
  - A batch that CI refused stays open: close it, or fix the track on the page and it re-publishes.
- **A flood:** check the top client hashes on the dashboard, then add a Cloudflare WAF rule (Cloudflare's logs have the address).
- **Turn it off:** `docker compose -p blinky-labels stop`.
- **Stop publishing but keep editing:** disable blinky-corpus's `labels-sync.yml` workflow (Actions → Labels sync → Disable).

## Retiring

1. Disable the deployer timer.
2. `docker compose -p blinky-labels down`.
3. `docker network rm blinky-labels-edge`, after removing it from the connector's stack.
4. `sudo ./firewall.sh remove`, then `systemctl disable blinky-labels-firewall.service`.
5. Delete the DNS record and the ingress rule, and disable the corpus's `labels-sync.yml`.
