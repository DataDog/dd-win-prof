---
name: dd-win-prof-release
description: Cut a release of dd-win-prof (Windows native profiler). Use when the user says "release vX.Y.Z of dd-win-prof", "cut a dd-win-prof release", "ship dd-win-prof", or "tag dd-win-prof vX.Y.Z".
---

# dd-win-prof release

`main` always sits at the version about to be released. So the flow is:
**tag first, bump after.** Two steps, then watch for the draft release.

```bash
git fetch origin && git checkout main && git pull --ff-only
```

## Step 1 — Sign and push the tag

```bash
git tag -s vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z
```

- `-s` produces a signed annotated tag. Lightweight unsigned tags are
  discouraged. If signing fails (e.g. missing key in ssh-agent), surface the
  error — do not silently fall back to `git tag` or `-c commit.gpgsign=false`.
- The workflow's first build step (`scripts/check-version.ps1`) asserts
  `version.h` matches the tag and fails fast otherwise.

## Step 2 — Bump `version.h` to next on a follow-up PR

Bump the minor (`Y`): e.g. `0.3.0` → `0.4.0`.

```bash
git checkout -b <user>/bump-next
# edit src/dd-win-prof/version.h: DLL_VERSION_MINOR -> next
git add src/dd-win-prof/version.h
git commit -m "Bump version.h to <next>"
git push -u origin <user>/bump-next
gh pr create --draft --title "Bump version.h to <next>"
```

## Step 3 — Look for the draft release

```bash
gh run watch -R DataDog/dd-win-prof \
  $(gh run list -R DataDog/dd-win-prof --workflow=release.yml -L 1 \
    --json databaseId -q '.[0].databaseId')

gh release view vX.Y.Z -R DataDog/dd-win-prof
```

The release is left as a **draft** on purpose. Edit the auto-generated
notes in the UI and publish manually.

## If something went wrong

Don't try to re-run the workflow against the same tag. Delete the release
and the tag, then start over from Step 1:

```bash
gh release delete vX.Y.Z -R DataDog/dd-win-prof -y
git push --delete origin vX.Y.Z
git tag -d vX.Y.Z
```

## References

- Workflow: `.github/workflows/release.yml`
- Octo-STS policy: `.github/chainguard/self.github.release.publish.sts.yaml`
- Local check: `pwsh scripts/check-version.ps1 -Tag vX.Y.Z`
- README sections: *Packaging a release*, *Cutting a release*
