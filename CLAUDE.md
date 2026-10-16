# Claude Code Instructions

Read AGENTS.md together with README.md and BUILDING.md before working on this
driver. Treat the Linux backend validation documented in docs/linux-rdna3.md as
a bounded result: Metal, IOKit and WindowServer still require macOS.

Use foreground, bounded commands for GPU smokes, retain their actual exit
statuses and inspect readback failures. A device enumeration or a successful
compile alone does not prove rendering. Preserve logs of failed attempts when
the failure identifies a build or runtime dependency.
