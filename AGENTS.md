# Reticulum C contributor instructions

- Use C17. Keep all untrusted inputs bounded and checked before copying or allocation.
- Public APIs use explicit ownership, typed status codes, caller-owned buffers, and opaque handles where appropriate.
- Builds must remain warning-clean with `RETICULUM_WARNINGS_AS_ERRORS=ON` under Clang and GCC.
- Use `apply_patch` for manual source edits. Preserve unrelated and user-authored working-tree changes.
- Make one focused Git commit for every major feature or bug fix. Include relevant tests and update `docs/FEATURE_STATUS.md` in the same commit.
- Do not claim interoperability, verification, or parity without bidirectional evidence against the recorded upstream revision.
- Never commit identities, private keys, message histories, captured private messages, generated local configurations, or build output.
- Before each checkpoint run:
  `cmake -S . -B build -G Ninja -DRETICULUM_BUILD_TESTS=ON -DRETICULUM_BUILD_APPS=ON -DRETICULUM_WARNINGS_AS_ERRORS=ON`,
  `cmake --build build`, and `ctest --test-dir build --output-on-failure` where socket permissions permit.

