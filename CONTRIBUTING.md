# Contributing to GCRecompiler

First off, thank you for considering contributing to GCRecompiler! It’s people like you who make this tool better for everyone.

To keep the project maintainable and the build checks green, please follow these guidelines.

---

## 🚀 Getting Started

1. **Fork the repository** and create your branch from `main` or `experimental`.
2. **Setup your environment**:
   * Windows 11, macOS, or Linux.
   * Ensure you have a C++ compiler supporting C++17 or higher.
   * Ensure **CMake** is installed locally.
3. **Run the build**: Verify you can compile the project locally using CMake before making changes.

---

## 🛠 Development Guidelines

### Build System & Dependencies
* This tool is written in **pure C++** with **no external dependencies or submodules**.
* All project build configurations must go through the provided `CMakeLists.txt`. Do not introduce platform-specific project configurations (like raw Visual Studio solutions or Xcode projects) to the repository.

### Code Style
* Use clear, descriptive variable names.
* Maintain the existing indentation and bracket style.
* Avoid platform-specific code unless absolutely necessary. Use standard preprocessor macros if you must handle OS differences.

### Commits
* Write concise, descriptive commit messages.
* Example: `Fix: Resolve memory leak in x86 recompiler core` or `Feat: Add support for Mac ARM64`.

### Workflow & Binaries
* **Do not commit binaries or build directories** (e.g., `.exe`, `.ds_store`, `build/`, or `out/`).
* Our GitHub Actions will automatically test your PR on Windows, Linux, and macOS. Ensure your PR passes all checks before requesting a review.

---

## 📬 Submitting Changes

1. **Push to your fork** and submit a **Pull Request**.
2. **Describe your changes**: Explain *what* you changed and *why*.
3. **Reference Issues**: If your PR fixes a specific bug, mention it (e.g., `Fixes #12`).
4. **Wait for CI**: Ensure the GitHub Actions "Build" and "Test" jobs turn green.

---

## ⚖️ License

By contributing, you agree that your contributions will be licensed under the project's **GPL 3.0 License**.

---

## 🚩 Reporting Bugs

If you find a bug, please open an **Issue** and include:
* Your Operating System.
* Steps to reproduce the bug.
* Expected vs. Actual behavior.
