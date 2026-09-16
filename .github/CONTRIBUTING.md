# Contributing

Thanks for your interest in improving github-secrets-watcher.

## Getting started

- Fork the repo and create a branch off `main`.
- Build the project with CMake:

  ```bash
  cmake -B build -DBUILD_TESTING=ON
  cmake --build build --parallel
  ctest --test-dir build --output-on-failure
  ```

  or with the included build script (see `build/build.sh` / `build/build.bat`).

## What we look for

- Fixes for real false positives/negatives in the scanner (excluded dirs, safe extensions, file patterns).
- Better safety around credentials and tokens (no leaking secrets in output/logs or clone URLs).
- Building on more platforms confirmations (libcurl + libgit2 are the only external deps).

## Guidelines

- Keep the token out of any string that could be logged or printed.
- Run `ctest` before opening a PR; new behavior should get a unit test under `tests/`.
- No unrelated formatting churn.

For anything else, open an issue and we will figure it out together.