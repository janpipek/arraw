# Security review

Date: 2026-10-03. Reviewer: GPT-6 (Codex).

Three actionable findings: two medium severity and one low severity. Production code was not changed.

## Scope and validation

Reviewed the local working tree, concentrating on untrusted photographs, XMP/JSON settings, metadata, export destinations, thumbnail caching, Python buffer ownership, and CLI output. Inspected relevant CPU/GPU boundaries and sandbox tooling as supporting context. This was a targeted manual review, not an exhaustive audit of every processing pass or third-party decoder.

HEAD was `e0c591826b6ae12f890704633164e899799603c3` at the start and advanced externally to `22134d37a5ec6aa865f2460ee26a370ff812fbd5` during review. Existing GUI/preview edits were left intact. Findings concern the files and line numbers linked below.

Runtime probes used the existing `build/container-debug/arraw-cli` and `build/container-py-debug/python/arraw` extension, with Qt 6.10.2 on Linux. These binaries were not rebuilt for this review; source inspection independently establishes the relevant paths. Probes used disposable directories, captured terminal output, disabled core dumps, and limited malicious CLI subprocesses to 1 GiB of address space and 12–20 seconds. No full test suite, sanitizer run, fuzz campaign, Windows/macOS validation, or dependency advisory audit was performed.

Threat model: an attacker can supply a photograph folder, including filenames and adjacent sidecars/symlinks, which a user opens, inspects, rates, edits, or exports. Impact is limited to the user's privileges; no privilege escalation or code execution was demonstrated.

## 1. Medium — Sidecar writes follow attacker-controlled symlinks outside the photograph folder

Locations: [Sidecar.cpp:709](/workspace/src/core/Sidecar.cpp:709), [Sidecar.cpp:774](/workspace/src/core/Sidecar.cpp:774), [MainWindow.cpp:592](/workspace/src/app/ui/MainWindow.cpp:592).

`writeSidecarFor()` uses `is_regular_file()` to decide whether to read an existing sidecar. That check follows symlinks. It then opens the original pathname with `QSaveFile`, which also follows the symlink for the write. There is no policy restricting the resolved target to the photo directory. Ordinary GUI rating actions reach this function through `writeSidecarMarks()` or the edit session.

An attacker who can plant `photo.xmp` as a symlink can redirect a user's sidecar write to another writable file. Existing targets must parse as acceptable XMP for this path to update them. A dangling symlink bypasses that parsing requirement: the code creates a fresh packet and writes it to the outside target. This allows creation of unintended files, or modification of unrelated XMP documents. It does not imply arbitrary bytes can be written or arbitrary existing non-XML files overwritten.

**Reproduced:** created `photos/photo.png` and `photos/photo.xmp -> ../outside.xmp`, with a valid RDF packet in `outside.xmp`. Calling `arraw.write_sidecar_marks(photo, arraw.PhotoMarks(rating=5))` changed the outside target to contain `Rating="5"` while retaining the symlink. Repeating with a dangling link to `../new-outside.xmp` created that outside file.

**Fix options:**

- Reject symlink sidecars and symlinked destination directories, and use platform-specific directory-relative operations that enforce that policy at open/commit time. A preliminary `symlink_status()` check alone leaves a replacement race.
- If linked sidecars are intentional, resolve and validate the target against an explicit allowed root, then perform the write through a stable directory handle. Cross-platform support needs a deliberate equivalent policy on Windows.

Add regression coverage for existing and dangling links, directory links, and concurrent replacement.

## 2. Medium — Unbounded sidecar depth and size permit denial of service

Locations: [Sidecar.cpp:104](/workspace/src/core/Sidecar.cpp:104), [Sidecar.cpp:209](/workspace/src/core/Sidecar.cpp:209), [Sidecar.cpp:464](/workspace/src/core/Sidecar.cpp:464), [Sidecar.cpp:478](/workspace/src/core/Sidecar.cpp:478). The second unbounded sidecar reader is `readSidecarXmp()` in [MetadataEmbedding.cpp](/workspace/src/core/MetadataEmbedding.cpp).

The settings reader reads the entire sidecar, builds a DOM, and recursively visits every element without a size, depth, or node budget. Namespace lookup walks ancestors for each element; nested elements with no default namespace declaration therefore also cause quadratic work in the application's traversal. Deep recursion can exhaust the stack, which exception handlers cannot recover from. Metadata export separately reads the entire XMP packet into a string without a size bound.

This is reachable through automatic sidecar loading, including `info`, photograph opening, and mark filtering. A small, well-formed malicious sidecar can make a normal inspection take seconds or longer; larger files can exhaust memory. GUI impact follows from the shared reader, although GUI hangs/crashes were not separately exercised.

**Reproduced:** beside a valid 61×41 PNG, wrote `'<a>' * depth + RDF_packet + '</a>' * depth`, where `RDF_packet` is `<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description/></rdf:RDF>`.

- At depth 10,000 (70,093 bytes), `arraw-cli info photo.png` succeeded but took about 6 seconds with an 8 MiB stack limit.
- At depth 20,000 (140,093 bytes), it exceeded a 20-second timeout with that stack limit.
- At depth 10,000 with a 1 MiB stack limit, it terminated with SIGSEGV (subprocess return code `-11`). A crash with the default 8 MiB stack was not demonstrated.
- Depth 40,000 (280,093 bytes) exceeded a 12-second timeout. Adding `xmlns=""` to each wrapper still timed out at depth 20,000, so not all observed cost can be attributed to the application's namespace walk.

**Fix options:**

- Bound bytes while reading, enforce depth/node limits with a streaming parser before constructing the DOM, and replace recursive traversal with an iterative traversal carrying namespace context. Apply the byte bound to both readers.
- Parse accepted XMP structures directly with a streaming parser under explicit resource budgets; preserve foreign content through a bounded representation. If full third-party metadata parsing must remain unrestricted, isolate it in a worker process with resource limits and cancellation.

A file-size check alone is insufficient for the demonstrated small nested packets. Add bounded-depth, bounded-size, and cancellation regression cases.

## 3. Low — Untrusted filenames and metadata inject terminal controls into text output

Locations: [InfoCommand.cpp:201](/workspace/src/cli/InfoCommand.cpp:201), [InfoCommand.cpp:444](/workspace/src/cli/InfoCommand.cpp:444), [InfoCommand.cpp:480](/workspace/src/cli/InfoCommand.cpp:480), [StreamDiagnostics.cpp:150](/workspace/src/cli/StreamDiagnostics.cpp:150).

`pathText()` normalizes UTF-8 but does not escape control characters. Text reports emit filenames, EXIF strings, and sidecar creator strings directly. Text diagnostics likewise emit paths and exception/diagnostic text unchanged. ANSI decoration does not sanitize the content, and disabling color does not prevent injected controls.

A supplied Linux filename or metadata string can clear or rewrite the terminal display, forge report/log lines, or invoke other controls the terminal implements. Clipboard manipulation depends on terminal support and configuration; it was not tested. No shell execution was demonstrated.

**Reproduced:** copied a valid PNG to a filename containing `evil\x1b[2J\x1b[H.png`, ran `arraw-cli info` with captured output, and confirmed the raw clear-screen/cursor-home sequence remained in stdout. The command returned 0. Capturing the output avoided executing those controls in the review terminal.

**Fix options:**

- Apply one shared text-output escaping function to every untrusted field, rendering ESC, CR, LF, other C0 controls, DEL, and relevant C1 controls visibly. Keep program-generated styling separate.
- Use escaped structured output for automation and introduce a safe quoted representation for all human-readable paths and metadata. `info --json --log-format json` is an available workaround for that command; JSON output already escapes the demonstrated ESC byte.

Add tests for controls in filenames, EXIF text, CreatorTool, and diagnostic messages, with color enabled and disabled.

## Other observations and limits

ImageBuffer performs checked allocation arithmetic, RAW copying verifies the sample count, Python array views retain their buffer owner, and exports use atomic save files. Export metadata is selected through allowlists rather than copied wholesale. These are useful protections, but atomic saving does not establish destination confinement, and parser exceptions do not handle stack exhaustion or bound resource use.

Potential improvements not promoted to additional findings include decoder process isolation, explicit image-processing memory budgets, and enforcing the CLI's no-overwrite promise atomically rather than checking destination existence before rendering. Their exploitability or failure behavior was not validated here. No claim is made that the linked LibRaw, Exiv2, Qt image plugins, or graphics drivers are free of vulnerabilities.
