"""Speech (or text) in, a compiled micro:bit program out.

The generator is constrained hard: the pinned template's DAL hash must not move,
so the prompt forbids touching `pxt.json`, packages or imports, and the source
is compiled against the same template the verified flash path already expects.

One retry only, with the compiler's own message fed back. A second failure is
returned honestly — the transcript, the source, and the compiler log — because
the device has to be able to show the real error rather than "try again".
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from .builder import Build, BuildError, build
from .config import Settings
from .providers import Generator

SYSTEM_PROMPT = """\
You write MakeCode for the BBC micro:bit v1 in TypeScript.

Rules, all mandatory:
- Reply with the contents of main.ts only. No prose, no markdown fences, no imports.
- Start with: const REVISION = "<three to six uppercase characters>"
  then call basic.showString(REVISION) so the running build is identifiable.
- Use only these APIs: basic.showString, basic.showNumber, basic.showLeds,
  basic.showIcon, basic.clearScreen, basic.pause, basic.forever,
  input.onButtonPressed, input.onGesture, input.onPinPressed, music.playTone,
  music.ringTone, bluetooth.startUartService.
- No packages, no `pxt.json` changes, no TypeScript features beyond `let`/`const`,
  functions, `if`, `for`, and arithmetic.
- Keep it under 30 lines. It must compile on the first attempt.
- If the request is ambiguous, show a short label of it and stop; never invent
  hardware that the micro:bit v1 does not have.
"""

MAX_ATTEMPTS = 2


class SpeakError(RuntimeError):
    """Raised when no attempt compiled. Carries what the device should display."""

    def __init__(self, transcript: str, source: str, log: str, attempts: int) -> None:
        super().__init__("the generated program did not compile")
        self.transcript = transcript
        self.source = source
        self.log = log
        self.attempts = attempts

    @property
    def compiler_message(self) -> str:
        """The compiler's own words, trimmed for a 480x480 panel."""
        return self.log.strip()[-600:] or "compiler produced no output"


@dataclass(frozen=True)
class SpeakResult:
    transcript: str
    source: str
    attempts: int
    build: Build
    compiler_logs: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        payload = {
            "transcript": self.transcript,
            "source": self.source,
            "attempts": self.attempts,
            "compiler_logs": self.compiler_logs,
        }
        payload.update(self.build.to_json())
        return payload


_FENCED = re.compile(r"```[A-Za-z]*\s*\n(.*?)```", re.DOTALL)
_STATEMENT = ("const ", "let ", "basic.", "input.", "music.", "function ")
_CODE_END = (";", "}", "{")


def extract_source(reply: str) -> str:
    """Return the program body: fenced block if present, else the code lines only.

    Generators like to add "Here you go:" before the code and "Hope that helps."
    after it; both must go, or the compiler sees prose.
    """
    fenced = _FENCED.search(reply)
    lines = (fenced.group(1) if fenced else reply).splitlines()

    start = next(
        (i for i, line in enumerate(lines) if line.lstrip().startswith(_STATEMENT)),
        None,
    )
    if start is None:
        raise ValueError("the reply contains no MakeCode statements")

    end = None
    for index in range(len(lines) - 1, start - 1, -1):
        stripped = lines[index].strip()
        if not stripped:
            continue
        if stripped.endswith(_CODE_END) or stripped.startswith(_STATEMENT + ("}", ")", "if ", "for ")):
            end = index
            break
    if end is None:
        raise ValueError("the reply contains no complete MakeCode statement")

    return "\n".join(lines[start : end + 1]).strip() + "\n"


def generate_program(
    transcript: str,
    *,
    generator: Generator,
    settings: Settings | None = None,
    build_fn=build,
) -> SpeakResult:
    """Turn a transcript into a compiled image, retrying once with the compiler log."""
    if not transcript.strip():
        raise ValueError("empty transcript")

    logs: list[str] = []
    source = ""
    prompt = transcript
    for attempt in range(1, MAX_ATTEMPTS + 1):
        reply = generator.complete(SYSTEM_PROMPT, prompt)
        source = extract_source(reply)
        try:
            result = build_fn(source, settings=settings)
        except BuildError as exc:
            logs.append(exc.log)
            if attempt == MAX_ATTEMPTS:
                raise SpeakError(transcript, source, exc.log, attempt) from exc
            prompt = (
                f"{transcript}\n\n"
                "Your previous answer failed to compile. Fix it and reply with main.ts only.\n"
                f"Program:\n{source}\n\nCompiler output:\n{exc.log[-2000:]}"
            )
            continue
        return SpeakResult(transcript=transcript, source=source, attempts=attempt, build=result, compiler_logs=logs)

    raise AssertionError("unreachable")
