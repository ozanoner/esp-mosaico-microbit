"""Configuration for the build service.

Every value can be overridden with an environment variable so the same image
runs on a workstation and on the always-on LAN box.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Settings:
    template_dir: Path = Path(os.environ.get("MB_TEMPLATE_DIR", REPO_ROOT / "microbit/template"))
    work_dir: Path = Path(os.environ.get("MB_WORK_DIR", REPO_ROOT / "build-service/.work"))
    mkc_bin: str = os.environ.get("MB_MKC_BIN", "mkc")
    toolchain_id: str = os.environ.get("MB_TOOLCHAIN_ID", "unpinned")
    build_timeout_s: int = int(os.environ.get("MB_BUILD_TIMEOUT_S", "180"))
    #: 8477 is deliberately not a common default (8080/8000/3000/5000), so a
    #: second service on the same LAN box does not collide with it.
    port: int = int(os.environ.get("MB_PORT", "8477"))

    def ensure_dirs(self) -> None:
        self.work_dir.mkdir(parents=True, exist_ok=True)


settings = Settings()
