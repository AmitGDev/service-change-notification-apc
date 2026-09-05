# GitHub CI/CD Infrastructure

This directory contains GitHub Actions workflows and reusable composite actions for automated CI/CD pipelines.

## Architecture

The CI/CD infrastructure follows a **"Recipe Pattern"** design:

* **Workflows** (recipes) define high-level orchestration
* **Composite Actions** (ingredients) encapsulate implementation details
* **Configuration** defines the analysis matrix and optional analysis features

This separation keeps workflows clean, maintainable, and reusable while keeping configuration independent from workflow implementation.

## Directory Structure

```text
.github/
├── workflows/
│   └── static-code-analysis.yml          # Main static analysis recipe
├── actions/
│   ├── process-analysis-config/
│   │   └── action.yml                    # Loads, parses, validates, and normalizes analysis configuration
│   ├── setup-cpp-dependencies/
│   │   └── action.yml                    # Installs and configures external C++ dependencies
│   ├── setup-cpp-environment/
│   │   └── action.yml                    # Installs LLVM, clang-tidy, Ninja, and configures the build environment
│   ├── check-formatting/
│   │   └── action.yml                    # Validates code formatting with clang-format
│   ├── configure-cmake/
│   │   └── action.yml                    # Configures the CMake build system
│   ├── build-project/
│   │   └── action.yml                    # Compiles the C++ project
│   └── run-clang-tidy/
│       └── action.yml                    # Runs clang-tidy static analysis
├── scripts/
│   └── run-clang-tidy.py                 # Python script for parallel clang-tidy execution
└── static-code-analysis.json              # Static analysis pipeline configuration
```

## Configuration

The static analysis workflow is configured through:

```text
.github/static-code-analysis.json
```

Example:

```json
{
  "os": [
    "windows-latest",
    "ubuntu-latest"
  ],
  "configurations": [
    "Debug",
    "Release"
  ],
  "codeql": true
}
```

### Configuration Properties

| Property         | Type    | Description                             |
| ---------------- | ------- | --------------------------------------- |
| `os`             | array   | GitHub Actions runner labels to analyze |
| `configurations` | array   | CMake build configurations              |
| `codeql`         | boolean | Enables or disables CodeQL analysis     |

The configuration is processed by the `process-analysis-config` composite action.

The action:

1. Loads the configuration file
2. Parses the JSON
3. Verifies required properties
4. Validates property values
5. Validates that arrays are not empty
6. Normalizes the configuration into workflow outputs

Invalid or incomplete configuration causes the `load-config` job to fail explicitly.

For example:

```text
Error: Configuration property 'configurations' is missing.
```

or:

```text
Error: Unsupported configuration 'Relase'.
Supported values: Debug, Release.
```

There are no implicit defaults. Configuration errors are intentionally detected before the analysis matrix is created.

## Workflows

### Static Code Analysis Recipe (`static-code-analysis.yml`)

**Purpose:** Cross-platform static analysis for C++23 code.

**Triggers:**

* Push to any branch
* Pull requests
* Weekly schedule (Sunday at 06:00 UTC)
* Manual dispatch

### Matrix Strategy

The matrix is generated dynamically from `.github/static-code-analysis.json`.

For example:

```json
{
  "os": [
    "windows-latest",
    "ubuntu-latest"
  ],
  "configurations": [
    "Debug",
    "Release"
  ]
}
```

produces four analysis jobs:

```text
Windows / Debug
Windows / Release
Ubuntu / Debug
Ubuntu / Release
```

The same mechanism also supports a single OS or single configuration without special handling.

### Pipeline

The workflow is structured into two phases:

#### 1. Process configuration

The `load-config` job:

1. Checks out only the configuration and the configuration-processing action
2. Processes and validates the configuration
3. Exposes the normalized values as job outputs

#### 2. Analyze

The `analyze` job creates the matrix from the configuration outputs and performs:

1. Checkout repository
2. Setup C++ dependencies
3. Setup C++ environment
4. Validate code formatting
5. Configure CMake with Ninja
6. Initialize CodeQL when enabled
7. Build the project
8. Verify the compilation database
9. Run clang-tidy with warnings-as-errors
10. Run CodeQL analysis when enabled
11. Upload build logs on failure
12. Upload `compile_commands.json`

## Composite Actions

### `process-analysis-config`

Loads and validates `.github/static-code-analysis.json`.

Responsibilities:

* Parse JSON configuration
* Validate required properties
* Validate OS values
* Validate build configurations
* Validate the CodeQL boolean
* Reject empty configuration arrays
* Produce JSON-array outputs suitable for GitHub Actions matrix expansion
* Produce the normalized CodeQL enable flag

**Platforms:** Ubuntu runner
**Inputs:** None
**Outputs:**

* `os` - JSON array of operating systems
* `configurations` - JSON array of build configurations
* `codeql` - `1` or `0`

The action intentionally does not provide defaults for missing configuration properties. A malformed or incomplete configuration should fail the workflow rather than silently change the requested analysis.

---

### `setup-cpp-dependencies`

Optionally installs and configures external libraries required to compile the project.

Examples include:

* Boost
* spdlog
* Other project-specific compilation dependencies

This action may be a no-op when the project has no external compilation dependencies. It keeps project dependency setup separate from CI/build tool setup.

---

### `setup-cpp-environment`

Installs and configures the C++ toolchain using LLVM 23.

**Windows:**

* MSVC latest environment via `msvc-dev-cmd`
* LLVM **23.0.1** installed from the official LLVM GitHub release
* clang-format
* clang-tidy
* Ninja

**Linux:**

* LLVM **23** tools installed using the runner's package repositories
* clang++
* clang-format
* clang-tidy
* Ninja

**Version policy:**

Windows uses the explicitly pinned LLVM version **23.0.1**.

Linux uses the LLVM **23** major version from the runner's package repositories rather than pinning a specific patch release.

The installation methods intentionally differ because the official Linux LLVM release is distributed as a large tar archive. Extracting the complete archive can exhaust the available disk space on the GitHub-hosted runner, even though the workflow requires only a small subset of the LLVM toolchain.

Linux therefore installs only the required packages.

Both CI environments use the same `.clang-tidy` and `.clang-format` configuration files and remain within the LLVM 23 toolchain family.

**Platforms:** Windows, Linux
**Input:** None
**Output:** LLVM and Ninja available to subsequent steps

---

### `check-formatting`

Validates C++ source files against the repository's clang-format rules.

The action fails the workflow when formatting issues are detected.

**Platforms:** Cross-platform
**Input:** None
**Output:** Exit code indicating formatting success or failure

---

### `configure-cmake`

Configures the CMake build system with the appropriate compiler for each platform.

* **Windows:** MSVC (`cl.exe`) with x64 architecture
* **Linux:** Clang (`clang++`)

The action uses Ninja so that CMake generates `compile_commands.json` consistently across platforms.

**Platforms:** Windows, Linux
**Input:** `config` (`Debug` or `Release`)
**Output:** Configured build directory

---

### `build-project`

Compiles the C++ project using CMake and Ninja.

* **Windows:** MSVC x64 toolchain
* **Linux:** Clang

**Platforms:** Windows, Linux
**Input:** `config` (`Debug` or `Release`)
**Output:** Compiled binaries and compilation database

---

### `run-clang-tidy`

Executes clang-tidy using the project's configuration.

* Parallel execution
* Uses all available CPU cores
* Warnings treated as errors
* Header filter: `source/.*`
* Source filter: `source/.*`

**Platforms:** Windows, Linux
**Input:** `build-dir` (default: `build`)
**Output:** Static analysis results

## CodeQL Analysis

CodeQL is controlled through `.github/static-code-analysis.json`.

Enable CodeQL:

```json
{
  "os": [
    "windows-latest",
    "ubuntu-latest"
  ],
  "configurations": [
    "Debug",
    "Release"
  ],
  "codeql": true
}
```

Disable CodeQL:

```json
{
  "os": [
    "windows-latest",
    "ubuntu-latest"
  ],
  "configurations": [
    "Debug",
    "Release"
  ],
  "codeql": false
}
```

No workflow environment variable is required.

## Customize clang-tidy Rules

Edit `.clang-tidy` in the repository root to configure clang-tidy checks.

## Modify the Analysis Matrix

The analysis matrix should be modified through `.github/static-code-analysis.json`, not directly in the workflow.

For example:

```json
{
  "os": [
    "windows-latest",
    "ubuntu-latest",
    "macos-latest"
  ],
  "configurations": [
    "Debug",
    "Release"
  ],
  "codeql": true
}
```

The configuration-processing action validates the values before the matrix is created.

Adding a new OS or configuration therefore requires updating the validation rules in `process-analysis-config` if the new value is not already supported.

## Usage

### Running Locally

**Check formatting:**

```bash
clang-format --dry-run --Werror source/**/*.{cpp,hpp,h}
```

**Fix formatting:**

```bash
clang-format -i source/**/*.{cpp,hpp,h}
```

**Run clang-tidy manually:**

```bash
cmake -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python .github/scripts/run-clang-tidy.py -p build "source/.*"
```

## Extending the Pipeline

To add a new workflow step:

1. If the step is simple and platform-independent, add it directly to the workflow.
2. If the step contains reusable or platform-specific implementation logic, create a composite action.
3. If the step introduces configuration, consider whether that configuration belongs in the central JSON configuration file.

Example composite action structure:

```yaml
name: "Action Name"
description: "What it does"

inputs:
  param-name:
    description: "Parameter description"
    required: true

runs:
  using: "composite"
  steps:
    - name: Step (Windows)
      if: runner.os == "Windows"
      shell: pwsh
      run: |
        # Windows implementation

    - name: Step (Linux)
      if: runner.os == "Linux"
      shell: bash
      run: |
        # Linux implementation
```

## Design Principles

1. **Separation of Concerns:** Workflows orchestrate; actions implement.
2. **Configuration Separation:** Pipeline configuration is external to workflow implementation.
3. **Cross-Platform by Default:** Write once, run across supported platforms.
4. **Fail Fast:** Invalid configuration and analysis failures are detected early.
5. **Explicit over Implicit:** Invalid configuration is rejected rather than silently defaulted.
6. **Artifact Preservation:** Preserve logs and compilation databases for debugging.
7. **Idempotent Actions:** Actions can be executed repeatedly without unintended side effects.
8. **Reusable Components:** Common CI functionality belongs in composite actions rather than individual workflows.

## Maintenance

### Updating LLVM

LLVM versions are maintained by `setup-cpp-environment`.

**Windows:**

```yaml
LLVM_VERSION: "23.0.1"
```

**Linux:**

```yaml
LLVM_MAJOR: "23"
```

Windows uses the official LLVM release with an explicit patch-version pin.

Linux uses the LLVM 23 major version from the runner's package repositories.

The installation methods intentionally differ. Windows uses the official LLVM MSI because it provides the required tools without extracting the complete LLVM distribution.

On Linux, extracting the complete official LLVM tar archive can exhaust the GitHub-hosted runner's available disk space. The workflow therefore installs only the required LLVM packages.

### Testing Changes

1. Create a feature branch.
2. Modify the workflow, action, or configuration.
3. Push changes to trigger the workflow.
4. Review the Actions tab for results.
5. Check uploaded artifacts if the build or analysis fails.

### Common Issues

**Issue:** `compile_commands.json not found`.

**Solution:** Ensure CMake is configured with:

```text
-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

The workflow uses Ninja so that CMake can generate the compilation database consistently, including when MSVC is used on Windows.

MSBuild does not provide the same compilation database generation behavior, which is why Ninja is used for the CI builds.

---

**Issue:** clang-tidy warnings fail the build.

**Solution:** Fix the warnings or adjust the `.clang-tidy` configuration.

---

**Issue:** Different clang-tidy patch versions on Windows and Linux.

**Solution:** This is expected.

Windows uses the pinned LLVM **23.0.1** release, while Linux uses the LLVM **23** major version from the runner's package repositories.

Both platforms remain within the LLVM 23 toolchain family.

---

**Issue:** The analysis matrix does not start.

**Solution:** Check `.github/static-code-analysis.json`.

The configuration-processing action validates:

* Required properties
* JSON syntax
* Non-empty arrays
* Supported OS values
* Supported build configurations
* CodeQL boolean type

Configuration errors are reported by the `load-config` job before matrix creation.

## Benefits of This Architecture

* Clean, high-level workflow
* Reusable composite actions
* Configuration independent from workflow implementation
* Dynamic analysis matrix
* Explicit configuration validation
* No platform-specific logic in the main workflow
* Consistent behavior across supported platforms
* Easier maintenance and extension
* Lightweight configuration-processing job using sparse checkout

## Contributing

When adding new workflows or actions:

1. Follow the recipe pattern.
2. Keep workflows focused on orchestration.
3. Use composite actions for reusable implementation logic.
4. Keep pipeline configuration in the appropriate configuration file.
5. Document inputs, outputs, and platform requirements.
6. Validate changes on all supported target platforms.
7. Update this README when the CI/CD architecture or behavior changes.

---

**Last Updated:** September 2026
**Maintainer:** Amit Gefen
