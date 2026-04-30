# FaceVeil 0.1.2

A maintenance release focused on safer batch processing, clearer review behavior, and basic automated test coverage.

## Security
- Custom ONNX model selection now checks for an existing `.onnx` file and rejects files larger than 512 MB
- Custom model loading now shows a trust warning before accepting the file
- SCRFD model validation now checks input tensor type/shape compatibility and output tensor element types before processing

## Stability
- Processing now refuses to start when multiple inputs would write to the same output path
- Preflight logging now reports the number of supported images found and confirms output path uniqueness
- Cancellation is checked between more processing stages, reducing the time between pressing Stop and the run ending

## UX
- Review mode now separates **Do Not Save** from **Copy Original**, avoiding accidental original-image output
- Closing a review dialog now cancels the whole run instead of implicitly saving
- Review-mode undo/redo hints now use the platform-native shortcut text

## Tests
- Added a CTest target covering supported image extensions, recursive image scanning, de-duplication, and mosaic behavior
- README now documents how to run the test suite

## Download

- **macOS (Apple Silicon)** — `FaceVeil-0.1.2-arm64.dmg`, signed and notarized. Requires macOS 12+.
- **Windows (x64)** — `FaceVeil-0.1.2-windows-x64.zip`. Unzip and run `FaceVeil.exe`. Requires Windows 10 or later.

---

# FaceVeil 0.1.1

A maintenance release: security, stability, and UX improvements. No feature changes.

## Security
- Path-traversal defence resolves symlinks inside the output tree
- Files > 1 GiB or images > 200 MP are skipped to prevent decode bombs
- Minimised macOS hardened-runtime entitlements
- Compiler hardening flags (`-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, PIE, MSVC `/guard:cf`)
- ONNX model output tensors are validated before decode

## Stability
- Atomic image writes (temp-file + rename)
- Cooperative thread shutdown; `QThread::terminate()` removed
- Directory scan skips permission errors and breaks symlink cycles
- Case-insensitive input de-duplication
- Refuses to run when the output folder is inside any input

## Performance
- SCRFD session is reused across runs with the same model
- Review dialog uses a downscaled preview (<= 1600 px long edge) for huge images
- Dropped a redundant `cvtColor` in BGR -> QImage conversion

## UX
- Per-image progress stages (Loading / Detecting / Reviewing / Applying mosaic / Saving)
- Undo / Redo in Review mode (Cmd+Z / Cmd+Shift+Z, 64-step history)
- Settings and window geometry persist across launches
- Drag-and-drop shows the correct cursor for invalid drops
- Activity log capped at 5,000 lines

## Packaging
- Windows script: qmake-based Qt detection, stricter dependency checks
- macOS script: dynamic Homebrew prefix, upfront signing-identity validation
- Notarization script: optional timeout, clearer failure messages

## Download

- **macOS (Apple Silicon)** — `FaceVeil-0.1.1-arm64.dmg`, signed and notarized. Requires macOS 12+.
- **Windows (x64)** — `FaceVeil-0.1.1-windows-x64.zip`. Unzip and run `FaceVeil.exe`. Requires Windows 10 or later.
