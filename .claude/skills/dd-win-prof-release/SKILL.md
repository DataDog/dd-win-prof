---
name: dd-win-prof-release
description: Cut a release of dd-win-prof (Windows native profiler). Use when the user says "release vX.Y.Z of dd-win-prof", "cut a dd-win-prof release", "ship dd-win-prof", or "tag dd-win-prof vX.Y.Z".
---

# dd-win-prof release

`main` holds the version about to be released: tag first, bump afterward.

## Tag and build

Start from an up-to-date, clean `main`. Check that
`src/dd-win-prof/version.h` matches the intended tag:

```bash
pwsh scripts/check-version.ps1 -Tag vX.Y.Z
git tag -s vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z
```

Use a signed annotated tag. If signing fails, surface the error; do not disable
signing. The [release workflow](../../../.github/workflows/release.yml) verifies
the version, stages the CMake install layout, and creates a draft release.

## Follow-up

Open a draft PR bumping `version.h` to the next development version. Populate
the repository PR template and include the Jira ticket in the title.

Review the generated release notes and publish the release manually after
validation. Do not publish it automatically.

## Recover a failed draft build

For an existing tag whose release is still unpublished, rerun the workflow
without deleting the tag or release:

```bash
gh workflow run release.yml -R DataDog/dd-win-prof -f release_tag=vX.Y.Z
```

The workflow replaces the zip if the release already exists. Do not use this
as a default recovery for published releases or overwrite published artifacts
without explicit approval.

## References

- [Packaging instructions](../../../README.md#packaging-and-releases)
- [Octo-STS policy](../../../.github/chainguard/self.github.release.publish.sts.yaml)
