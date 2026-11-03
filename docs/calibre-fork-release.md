# Releasing the Calibre fork

The fork follows upstream's channels: Classic is the stable release, and New UI
is a prerelease. LCD 3.49 boards find updates through the catalog on this fork's
GitHub Pages. Other boards use `releases/latest`.

Never run `git push --tags`: a local clone can hold upstream tags, and each one
would run its own commit's release workflow in this repository.

## Classic

1. Merge the upstream tag into `calibre` with a merge commit, and add
   `docs/releases/<tag>-calibre.md`.
2. Push an annotated `vX.Y.Z-calibre[.N]` tag that is on `calibre`.
3. `release.yml` builds every board, publishes the release as Latest and starts
   `pages.yml`, which refreshes the catalog.

## New UI

The catalog takes one tag per board, and each tag carries one board's descriptor.

1. On `calibre-new-ui`, run the **New UI candidate** workflow twice: `lcd349-v1`
   with `v0.2.0-calibre`, and `lcd349-v2` with `v0.2.0-calibre.1`.
2. Test each image on hardware.
3. Publish each tag with its candidate's assets, `update-channel.json` and
   `SHA256SUMS.txt`, always as a prerelease. Without that flag the release can
   become Latest and reach boards on the old update path.
4. Set the repository variables `RSVP_UI_PREVIEW_TAG` and
   `RSVP_UI_REV2_PREVIEW_TAG`, and run `pages.yml`. Publish Classic first, so
   **Return to Classic** has a target.
