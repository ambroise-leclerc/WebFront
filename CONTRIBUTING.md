# Contributing Guidelines

## Coding Style

We follow the **C++ Core Guidelines** to ensure conformance with modern C++23 generic programming. Please refer to the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) for detailed rules. Key points are summarized below:

### Naming Conventions

- **Classes/Structs:**
  - Use `UpperCamelCase` (e.g., `MyClass`, `DataProcessor`).
- **Functions/Methods:**
  - Use `lowerCamelCase` (e.g., `processData()`, `getValue()`).
- **Variables (including const, constexpr, and constinit variables):**
  - Use `lowerCamelCase` (e.g., `dataBuffer`, `isReady`).
- **Namespaces:**
  - Use 'lowercase' (e.g., 'mui', 'backend').
- **Macros:**
  - Use `ALL_CAPS_WITH_UNDERSCORES`. But do not use macros.

### Formatting

- Indent with 4 spaces, no tabs.
- Place pointer/reference symbols next to the type (e.g., `int* ptr`, `const std::string& name`).
- Use `nullptr` instead of `NULL`.

### File Organization

- Header files: `.hpp`
- Source files: `.cpp`
- **File naming:** Use `UpperCamelCase` for `.hpp` and `.cpp` files (e.g., `Logger.hpp`, `DataProcessor.cpp`).

### Documentation

We use **Doxygen** syntax for code documentation. Follow these guidelines:

- **File-level documentation:** Use `@brief` only, no `@file` or `@author` tags.
- **Class documentation:** Include `@brief` with detailed description and usage examples.
- **Method documentation:**
  - Use compact notation `/** @brief Description */` for simple one-line descriptions.
  - Use full format with `@param`, `@return`, `@note` for complex methods.
  - Include usage examples with `@code` blocks when helpful.
- **Template parameters:** Document with `@tparam` when non-obvious.
- **Private members:** Generally no documentation needed unless complex.

Example formats:
```cpp
/** @brief Simple one-line description */
void simpleFunction();

/**
 * @brief Complex function with detailed documentation
 *
 * Detailed description of what the function does, including
 * important implementation details and usage patterns.
 *
 * @param param1 Description of first parameter
 * @param param2 Description of second parameter
 * @return Description of return value
 *
 * @note Important notes about usage or behavior
 *
 * @code
 * // Usage example
 * auto result = complexFunction(value1, value2);
 * @endcode
 */
ReturnType complexFunction(Type1 param1, Type2 param2);
```
## Branching and Project Flow

The `master` branch contains the stable, versioned, and deployable state of the project. Only tested and production-ready code is merged here.

All new features, bug fixes, and changes are developed in feature branches, which are named after the corresponding issue number (e.g., `123-feature-description`).

Feature branches are merged into the `develop` branch, which serves as the integration branch for ongoing development. The `develop` branch collects all completed features and fixes before they are considered stable.

Periodically, when the code in `develop` is stable and ready for release, it is merged into `master` and tagged with a new version.

Hotfixes for production issues may be branched from `master`, then merged back into both `master` and `develop` to keep all branches up to date.

This workflow ensures that `master` always reflects the latest deployable version, while `develop` is used for active development and integration.

## Tooling

To ensure consistency, we use `.clang-format` and `.clang-tidy` to enforce our coding style. These configuration files are included in the root of the repository.

- **Clang-Format:** Automatically formats your code to match our style guidelines.
- **Clang-Tidy:** Detects and warns about style violations, bugs, and non-modern C++ practices.

Please run these tools on your code before submitting a pull request.

## Pull Requests

- **One commit per pull request.**
- The pull request (PR) title must reference the related issue or feature (e.g., `Add CameraManager class [#42]`).
- Provide a clear description of the changes and the motivation.
- Ensure your branch is up to date with `develop` before submitting.
- All code must pass CI checks and tests before merging.
- Request a review from at least one maintainer.

## Additional Notes

- Write clear, descriptive commit messages.
- Add or update documentation as needed.
- Run all tests locally before submitting your PR.

