# Build box image: pinned MakeCode compiler + frozen Python environment.
#
# Everything that could drift is resolved at image build time:
#   * node base image by digest
#   * mkc (the MakeCode compiler) by exact npm version, from a committed lock
#   * Python dependencies from the committed uv.lock
#   * the MakeCode editor worker that mkc downloads (layered into /root/.pxt)
#
# At runtime the container needs no network to build a payload.
FROM node:22-bookworm-slim@sha256:de170b39439eb0e5a3444ad7d60b015614a381e10fecae2e30ec9e735122f38d

ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONUNBUFFERED=1 \
    PATH="/opt/mkc/node_modules/.bin:${PATH}" \
    UV_CACHE_DIR=/opt/uv-cache \
    UV_PROJECT_ENVIRONMENT=/opt/venv \
    UV_PYTHON_PREFERENCE=only-system \
    UV_LINK_MODE=copy \
    MB_TOOLCHAIN_ID=mkc-1.3.6 \
    MB_TEMPLATE_DIR=/workspace/microbit/template \
    MB_WORK_DIR=/tmp/mb-work

RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 python3-venv ca-certificates git \
 && rm -rf /var/lib/apt/lists/*

COPY --from=ghcr.io/astral-sh/uv:0.11.29 /uv /usr/local/bin/uv

# --- MakeCode compiler, exact version -------------------------------------
WORKDIR /opt/mkc
COPY build-service/package.json build-service/package-lock.json ./
RUN npm ci --omit=dev --ignore-scripts \
 && npm ls --depth=0 \
 && mkc --version

# --- Python environment, frozen lock --------------------------------------
# The project sources are mounted at runtime, so only the third-party
# dependencies are resolved here; the entrypoint syncs the mounted project
# against the same lock, offline, using this cache.
WORKDIR /opt/build-service
COPY tools/hexkit /opt/tools/hexkit
COPY build-service/pyproject.toml build-service/uv.lock ./
RUN uv sync --frozen --extra dev --no-install-project

# --- warm the MakeCode editor cache so runtime builds stay offline --------
# mkc fetches parts of a released editor on first use; doing it here freezes
# them into the image instead of letting them drift between runs.
COPY microbit/template /tmp/warm-template
RUN mkdir -p /tmp/warm-project \
 && cp /tmp/warm-template/pxt.json /tmp/warm-project/pxt.json \
 && printf 'basic.showString("W")\n' > /tmp/warm-project/main.ts \
 && cd /tmp/warm-project \
 && mkc build \
 && rm -rf /tmp/warm-project /tmp/warm-template \
 && test -d /root/.pxt/mkc-cache \
 && du -sh /root/.pxt

WORKDIR /workspace
COPY build-service/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["serve"]
