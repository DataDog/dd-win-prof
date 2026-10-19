# Contributing

For bugs, include reproduction steps, expected/actual behavior, Windows version,
and build configuration. Discuss substantial feature or architecture changes
with the maintainers before implementing them.

## Development

Start with the [build instructions](README.md#build). Test commands live beside
the tests:

- [Unit tests and ASan](src/Tests/README.md)
- [RUM integration tests](src/integration-tests/README.md)
- [Profile correctness and symbol validation](e2e-tests/README.md)
- [Obfuscation tests](obfuscation/readme.md#build-and-test)
- [Vulkan example builds](e2e-tests/README_vulkan.md)

Keep temporary scripts and diagnostics under the git-ignored `.scratch/`
directory, not alongside committed tests.

## Pull requests

- Branch from `main` and follow the existing code style.
- Add focused tests for behavior changes and update affected documentation.
- Describe the motivation and validation in the pull request template.
- Code-owner review is required before merging; see [CODEOWNERS](CODEOWNERS).
