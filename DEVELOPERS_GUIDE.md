# Developers Guide

## CMake Presets

This project uses CMake presets for easy configuration in VSCode and command line. The default preset uses **RelWithDebInfo** build type for development.

### Available Presets

- **dev** (default) - Development build with optimizations + debug info (RelWithDebInfo)
- **dev-python** - Development build with Python bindings enabled
- **debug** - Full debug build with verbose logging
- **release** - Optimized release build
- **shared-only** - Build only shared library
- **static-only** - Build only static library
- **tests** - Build with tests enabled

### Using Presets

**VSCode**: The CMake Tools extension will automatically detect and use presets. Select your preset from the CMake status bar.

**Command Line**:
```bash
# Configure with a preset
cmake --preset dev

# Build with a preset
cmake --build --preset dev

# Or use the traditional approach after configure
cd build && ninja
```

### Build Options

All presets support these CMake options:
- `DFTRACER_UTILS_BUILD_SHARED` - Build shared library (default: ON)
- `DFTRACER_UTILS_BUILD_STATIC` - Build static library (default: ON)
- `DFTRACER_UTILS_BUILD_BINARIES` - Build command-line tools (default: ON)
- `DFTRACER_UTILS_BUILD_PYTHON` - Build Python bindings (default: OFF)
- `DFTRACER_UTILS_DEBUG` - Enable verbose debug logging (default: OFF)
- `DFTRACER_UTILS_TESTS` - Build tests (default: OFF)

## Tests

```bash
make test
```

## Code Coverage

This project supports code coverage reporting using lcov/gcov with automatic Ninja build system detection.

### Prerequisites for Coverage

Install lcov:
- **macOS**: `brew install lcov`
- **Ubuntu/Debian**: `sudo apt-get install lcov`
- **CentOS/RHEL**: `sudo yum install lcov`

Optional (for faster builds):
- **Ninja**: `brew install ninja` (macOS) or `sudo apt-get install ninja-build` (Ubuntu)

### Generate Coverage Report

Using Make:
```bash
make coverage              # Generate coverage report
make coverage-open         # Generate and open in browser
make coverage-view         # Open existing report
make coverage-clean        # Clean coverage artifacts
```

Using the script directly:
```bash
./scripts/coverage.sh              # Generate coverage report
./scripts/coverage.sh --open       # Generate and open in browser
./scripts/coverage.sh --help       # Show all options
```

The coverage report will be generated in `coverage/html/index.html`.

### Build System Detection

The coverage script automatically detects and uses Ninja if available, falling back to Make otherwise. This provides faster builds when Ninja is installed.

## Python Linting and Type Checking

This project uses [ruff](https://docs.astral.sh/ruff/) for linting/formatting and [ty](https://docs.astral.sh/ty/) for type checking. Both are run via `uvx` (no install needed):

```bash
make lint        # ruff check + format check
make typecheck   # ty type check
```

Or directly:
```bash
uvx ruff check python/ tests/python/
uvx ruff format --check python/ tests/python/
uvx ty check python/
```

These checks run automatically in the pre-commit hook when Python files are staged. They also run in CI for Python 3.9+ (ruff) and 3.12+ (ty).

Configuration is in `pyproject.toml` under `[tool.ruff]`.

## Git Hooks

Install the project's pre-commit hooks:

```bash
./scripts/git-hooks.sh install
```

The pre-commit hook runs:
- **C/C++**: `clang-format` on staged `.c/.cpp/.h/.hpp` files
- **Python**: `ruff check`, `ruff format --check`, and `ty check` on staged `.py/.pyi` files (requires `uvx` or `ruff` in PATH; skipped gracefully if not available)
- **Web UI**: `prettier`, `eslint`, and `tsc` type-check when `web/` files are staged (requires Node/`npm` and `web/node_modules`; skipped gracefully if either is missing)

## Web UI

The trace viewer web UI lives in `web/` (SolidJS + Vite). It is built into two
self-contained pages that are embedded into the `dftracer_server` binary at
compile time: `dist/index.html` (the timeline viewer, served at `/`) and
`dist/api.html` (the API explorer, served at `/api`).

```bash
cd web
npm ci
npm run build         # -> web/dist/index.html and web/dist/api.html
```

`web/dist/` is git-ignored. The C++ build embeds whatever is in `web/dist/` via
`cmake/scripts/embed_asset.cmake`; if it is absent, a placeholder page is
embedded instead (the `/api/v1` REST API works regardless), so **the C++ build
never requires Node**. Rebuild the pages after changing the UI, then rebuild the
server. To have CMake build the UI for you, configure with
`-DDFTRACER_UTILS_BUILD_WEB_UI=ON` (needs `npm`) or run the `web-ui` target.

Development and checks:

```bash
npm run dev           # Vite dev server, proxies /api to http://localhost:8080
npm run typecheck     # tsc --noEmit
npm run lint          # eslint
npm run format        # prettier --write .
npm run format:check  # prettier --check .
```

These same checks run in the pre-commit hook (above) and in CI.

## Make Targets

Run `make help` to see all available targets:

- `make coverage` - Build with coverage and generate HTML report
- `make coverage-open` - Build coverage and open report in browser
- `make coverage-clean` - Clean coverage build directory
- `make coverage-view` - Open existing coverage report in browser
- `make test` - Build and run tests without coverage (uses Ninja if available)
- `make test-coverage` - Run tests with coverage (requires prior coverage build)
- `make test-py` - Run Python tests in isolated environment
- `make lint` - Run ruff linter on Python code
- `make typecheck` - Run ty type checker on Python code
- `make format` - Format code using clang-format
- `make check-format` - Check code formatting
- `make cmake-format` - Format CMake files
- `make clean` - Clean all build artifacts
