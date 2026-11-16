# CONTRIBUTION - ᴇɴ

Thank you for considering contributing to the **HTTP Caching Proxy Server**! This guide covers setting up your environment, the project's conventions and how to submit your work.

- [How to build and run](docs/README.md)
- [Architecture](docs/ARCHITECTURE.en.md)
- [Versão em português](CONTRIBUTING.pt-br.md)

## Project Structure

```
.
├── .github/workflows/ci.yml   # CI: build + tests + smoke test (GCC/Clang, with and without sanitizers)
├── CMakeLists.txt             # proxy_core library, http_proxy executable, proxy_tests
├── .clang-format              # code style
├── docs/                      # how to run and architecture (en + pt-br)
├── scripts/smoke_test.sh      # end-to-end test against a local origin
├── src/
│   ├── main.cpp               # CLI, thread pool, signals
│   ├── util/                  # strings, logger
│   ├── http/                  # messages, parser, URLs, dates (no I/O)
│   ├── cache/                 # LRU, Cache-Control, RFC 9111 policy
│   └── proxy/                 # server, session, upstream, tunnel, deadline, config
└── tests/                     # Catch2 unit tests (one file per module)
```

Layers only depend on the layers below them: `proxy → cache → http → util`. Code in `http/` and `cache/` does no I/O, so it can be tested without a network. See the [architecture](docs/ARCHITECTURE.en.md) for details.

## Getting started

1. Fork and clone the repository.
2. Install the dependencies (see [prerequisites](docs/README.md#1-prerequisites)).
3. Build with sanitizers and run the tests:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPROXY_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
scripts/smoke_test.sh build-asan/http_proxy
```

4. Create a branch from `main`:

```bash
git checkout -b feat/my-change
```

Suggested prefixes: `feat/`, `fix/`, `docs/`, `test/`, `refactor/`, `ci/`.

## General Rules

- **C++17**, no compiler extensions. Do not add dependencies beyond Boost (headers) and Catch2 without discussing it in an issue first.
- **No warnings.** The project builds with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, and CI treats warnings as errors.
- **Formatting:** follow `.clang-format`. To format only the lines you changed:

```bash
git clang-format main
```

- **Naming:** types in `PascalCase` (`HttpCache`), functions and variables in `snake_case` (`parse_cache_control`), private members with a trailing `_` (`buffer_`), constants prefixed with `k` (`kViaToken`).
- **Namespaces:** all code lives in `proxy::` and the module's sub-namespace (`proxy::http`, `proxy::cache`, `proxy::log`, `proxy::util`). File-local helpers go into an anonymous namespace.
- **Headers:** `#pragma once`; in a `.cpp`, include its own header first, then the standard library, Boost and finally project headers (path relative to `src/`, e.g. `#include "http/parser.hpp"`).
- **Concurrency:**
  - a session's state is only touched from its strand, without locks;
  - shared state lives in `ProxyContext` and must be thread-safe (`std::atomic` or an internal mutex, as in `HttpCache`);
  - objects with asynchronous operations derive from `enable_shared_from_this` and capture `shared_from_this()` in their handlers;
  - use `Deadline` for timeouts instead of adding new timers.
- **Errors:** I/O uses `boost::system::error_code`, not exceptions. Exceptions are only for startup (e.g. a failed `bind`).
- **Logging:** use `LOG_DEBUG/INFO/WARN/ERROR` with the session id (`"[" << id_ << "] ..."`). `INFO` is reserved for one line per request and server events.
- **RFCs:** when implementing HTTP or caching behaviour, cite the RFC section in a comment (e.g. `// RFC 9111 §4.2.3`).

### 2. Tests

- Every bug fix should come with a test that fails without the fix.
- New features in `http/` or `cache/` need unit tests in the matching `tests/` file. A new test file must be added to `proxy_tests` in `CMakeLists.txt`.
- Use one Catch2 tag per module (`[parser]`, `[url]`, `[http-cache]`, ...).
- Changes to network behaviour (`proxy/`) should be checked with the smoke test; when practical, extend `scripts/smoke_test.sh` with the new case.
- Run the tests with sanitizers before opening a PR.

### 3. Commits

Use [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<optional scope>): <imperative description>
```

Examples:

```
feat(cache): support stale-while-revalidate
fix(parser): reject overflowing chunk size
docs: explain the strand model
test(url): IPv6 with port cases
```

Types: `feat`, `fix`, `docs`, `test`, `refactor`, `perf`, `ci`, `build`, `chore`. Keep each commit focused on one change.

### 4. Pull Requests

1. Bring your branch up to date with `main` before opening the PR.
2. Make sure CI passes: all four jobs (GCC/Clang × Release/Sanitizers) must be green.
3. In the description, explain **what** changed and **why**, how it was tested and, when relevant, the related RFC section. Reference the issue (`Closes #N`).
4. Small PRs get reviewed faster. Keep refactorings separate from behaviour changes.
5. Address review comments with new commits; history can be squashed on merge.

Checklist:

- [ ] builds without warnings on GCC and Clang
- [ ] `ctest` and `scripts/smoke_test.sh` pass, including with `-DPROXY_ENABLE_SANITIZERS=ON`
- [ ] new tests cover the change
- [ ] documentation updated (README, `docs/`, `--help`) for user-visible changes

### 5. Documentation

- Documentation is bilingual: every change to a `*.md` file must also be made in the other language (`README.md` ↔ `README.pt-br.md`, `docs/ARCHITECTURE.en.md` ↔ `docs/ARCHITECTURE.pt-br.md`, etc.).
- A new command-line option must appear in `usage()` (`src/proxy/config.cpp`) and in the options table of `docs/README*.md`.
- Changes to the request flow, concurrency or caching policy must be reflected in `docs/ARCHITECTURE*.md`, including the Mermaid diagrams.
- Code comments are in English and explain **why**, not what the line does.

### 6. Issues

- **Bugs:** include the command used to start the proxy, the request (e.g. the `curl` command), the expected and actual results, and the relevant log lines with `--log-level debug`.
- **Feature requests:** describe the use case and, if possible, the RFC reference.
- **Security:** do not open public issues for vulnerabilities; contact the maintainer directly.

by caiothedev<br />
#contribution