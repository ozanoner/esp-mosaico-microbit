# Architecture diagrams

Two diagrams, both drawn from this tree — every box cites the file and line range it
came from, and the validator checks those ranges against the pinned commit.

| diagram | JSON (the source) | static | interactive |
|---|---|---|---|
| Solution view — the whole chain, board to build box | `solution.json` | `solution.png` | `solution.html` |
| Component view — the monorepo's parts and their contracts | `components.json` | `components.png` | `components.html` |

The JSON is the source of truth. Do not edit the HTML: the next `finalize` overwrites it.

## Regenerating

Pinned tool: **archify 3.1.0** (`~/.hermes/profiles/esp32-developer/skills/archify`,
tag `v3.1.0`). Node >= 18 and a Chrome/Chromium on `PATH` are required — where
unprivileged user namespaces are restricted, the browser gate needs a `--no-sandbox`
shim, otherwise it dies with `No usable sandbox!`.

```sh
A=~/.hermes/profiles/esp32-developer/skills/archify

# 1. re-pin the revision in meta.repository (every diagram must name the tree it describes)
git rev-parse HEAD

# 2. author or adjust the JSON, then run every gate against this tree
node "$A/bin/archify.mjs" finalize architecture docs/architecture/solution.json \
     docs/architecture/solution.html --repo-root . --quality showcase

# 3. static render for GitHub (it will not display an .html blob)
python3 <skill>/scripts/render_archify_png.py docs/architecture/solution.html \
        docs/architecture/solution.png 2
```

`--repo-root .` is what makes the evidence check run. A non-zero exit is a failed
gate, never a warning: read the diagnostics, fix the **JSON**, rerun. Once a run has
passed, the HTML owns its browser evidence, so a rerun needs its own
`--out-dir docs/architecture/review-<n>`.

## What the gate cannot check

`validate` checks JSON shape, `check` checks provenance, `browser-check` measures
layout in a real browser. **None of them checks whether a sentence is true.** The
labels, versions and claims are only as good as the `sources` on each component, so
re-read the cited range when you edit one.

## Receipts are not committed

`finalize` writes `*.finalize*.json`, `*.browser-check.json`, `*.delivery.json` and
`review-*/` next to the diagram. They carry absolute paths, a machine-specific Chrome
path and timestamps, so they are gitignored — they regenerate on every run.
