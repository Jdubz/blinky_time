# The labels service (README.md). Build context: this directory.
#
#   docker build --build-arg LABELS_SERVICE_VERSION=$(git rev-parse HEAD:labels-service) -t labels-service labels-service
#
# Standard library Python plus git and ssh (the service's own corpus clone, and pushing label
# batches with its deploy key to ssh.github.com:443). Runs as uid 10001 and works with a read-only
# root filesystem: everything it writes goes to /corpus (the clone), /state (the shared labels, the
# publisher's state) and /tmp.
FROM python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c

RUN apt-get update \
 && apt-get install -y --no-install-recommends git openssh-client ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --uid 10001 --no-create-home --home-dir /tmp --shell /usr/sbin/nologin reviewer \
 && mkdir -p /corpus /state \
 && chown 10001:10001 /corpus /state

COPY server.py state.py hosted.py labelcheck.py seeds.json index.html github_known_hosts /app/

# The git tree hash of labels-service/: /api/config reports it, and the site stamps the same hash
# into the page, which asks for a reload when they differ.
ARG LABELS_SERVICE_VERSION=dev
ENV LABELS_SERVICE_VERSION=${LABELS_SERVICE_VERSION} \
    PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 HOME=/tmp GIT_TERMINAL_PROMPT=0
USER 10001:10001
EXPOSE 8766
# Named volumes on these paths start out owned by 10001 (Docker copies the image's ownership).
VOLUME ["/corpus", "/state"]
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD ["python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8766/healthz', timeout=4)"]
ENTRYPOINT ["python", "/app/server.py"]
