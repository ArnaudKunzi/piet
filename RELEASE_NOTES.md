# PIET A.0.0 / CoBrA 1.0.0 — public source cut of 2026-11-06

This cut replaces the experimental 2026-10 source beta with the full current
source tree of PIET A.0.0 and CoBrA 1.0.0 (the compiled version strings), as
prepared for public distribution on 2026-11-06. The whole tree is replaced in
one commit on top of the beta's own public history; no private development
history, private oracle logs, licensed reference datasets, or internal
automation is included.

- Every statistical routine ships as the project's own implementation. Code
  adapted from GPL-compatible open-source projects is declared in
  `THIRD_PARTY_NOTICES.txt` and in a header comment of the file that carries it.
- Tests and self-test arms that depend on private oracle fixtures report
  themselves as declared skips in a public clone; the fixtures they name are
  not required to build or use the software.
- Corresponding source for any binary published on the release page is this
  repository at the release tag.

# PIET 1.0 Beta 1 — v1.0.0-beta.1 (historical)

The first public source snapshot (2026-10-12) with an Apple Silicon DMG. It
used a different version naming scheme and withheld several functions pending
provenance review; those functions are present in the current tree as the
project's own implementations.
