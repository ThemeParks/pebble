<!-- Thanks for contributing! A few notes before you submit:

- If this is a non-trivial change, please link to the issue discussing it.
- One concern per PR. Keep refactors separate from feature changes.
- Make sure `./pebble test` and `./pebble test --dest all` both pass.
-->

## What this changes

<!-- One or two sentences. -->

## Why

<!-- The motivation, or link to the issue that explains it: Fixes #NNN -->

## How to test

<!-- Steps the reviewer can take to verify the change works.
Include the exact commands if the change affects build/test flow. -->

```
./pebble build wdw
./pebble test
```

## Checklist

- [ ] I've run `./pebble test` locally (WDW integration tests: 7 passed)
- [ ] I've run `./pebble test --dest all` if my change touches the uber app (12 passed)
- [ ] I've added tests for new behaviour (if applicable)
- [ ] I've updated docs (README, CONTRIBUTING, comments) if needed
- [ ] Commit messages follow the project style (see CONTRIBUTING.md)

## Screenshots

<!-- If you changed UI, include before/after screenshots. Emery + gabbro if the change affects layout. -->
