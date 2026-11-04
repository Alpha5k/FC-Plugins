# Style

Use simple C++23 and standard library facilities where they fit. Keep abstractions small and tied to a concrete need.

## Names

| Item | Convention |
| --- | --- |
| Types and enum values | PascalCase |
| Functions, fields, variables, namespaces, source files and folders | snake_case |
| Private data members | Trailing underscore |
| Named constants | kPascalCase |
| Project macros | FC_UPPER_SNAKE_CASE |

Treat acronyms as words: `PluginApi`, `CpuContext`, `Rva`. Preserve external API names and conventional tool filenames.
Lua follows the Mod Tools naming conventions described below.

Fusion Cutter multiplayer feature IDs use `FC.PascalCase`, for example `FC.AerialGravity`.
Configuration option names use PascalCase, with explicit unit suffixes such as `MaxTraceSizeMB`.

## Formatting

Use clang-format 22 with the repository's `.clang-format` file. From the repository root:

```powershell
clang-format -i <files>
```

Separate definitions and logical steps with a single blank line. Keep related statements together and comments
beside the code they explain. Use UTF-8 without BOM, LF line endings and a final newline.

## C++

- Use `.hpp` for C++ headers, `.h` for C interfaces and `#pragma once`. Include the corresponding header first
  in implementation files. Headers include their own dependencies; public headers must not depend on private ones.
- Keep `using namespace` out of headers and implementation details local to their source files.
- Use scoped resource ownership and ordinary C++ internally, with the specified C representation at ABI boundaries.
- Select patch behavior by game build. Use architecture conditions for machine differences.
- Return expected failures through the normal error paths; reserve assertions for internal invariants.
- Use `[[nodiscard]]` for results that must be checked to avoid corrupting state, leaking resources or missing essential errors.
- Keep compiler warning settings local to project targets and explain warning suppressions.

## Patches and plugins

- Keep patch implementations and public contracts under `patches/`, plugin composition under `plugins/`, and addon
  scripts and assets under `addons/`.
- Use the public FC SDK and published patch APIs. Use FC's Game, Multiplayer, Lobby, Lua and tracing facilities
  for the services they provide.
- Declare supported builds and roles through variants. Compile native implementations only for architectures
  with supported variants.
- Follow callback thread and lifetime requirements. Copy borrowed data before retaining it, and distinguish FC peer IDs
  from native player slots and indices.
- Keep diagnostics passive. Add capture settings when they provide a useful choice about the data collected or its cost.
- Keep patch IDs, configuration keys and protocol meanings consistent. Update affected consumers when changing a contract.

### Native layouts

- Keep layout types and selectors in `layout.hpp` and `layout.cpp`, or locally in `patch.cpp` for a small patch.
  Keep verified addresses, calling conventions and necessary evidence with the layout.
- Use `select_layout(profile)` returning `std::optional<Layout>`. Use more specific type names when a patch needs
  distinct layouts. Return `nullopt` for unsupported profiles.
- For multiple builds, use a private `kProfiles` table with a `profile` field and named initializers. Match profiles
  exactly; keep shared construction in `make_layout` and differing addresses and bytes in the table rows.
- Handlers use the selected layout. Fail planning when a required layout is unavailable and report the selected
  image profile.
- Check only the evidence needed for an operation, over the smallest relevant range. Masked bytes still reserve
  their range and can conflict with other patches.

## Lua

- Match existing screen function and field names. Prefix new helpers with their screen or component name.
- Use tabs, readable tables and blank lines between logical steps.
- Preserve previous callbacks and forward their arguments and results when extending behavior.
- Keep readable Lua and munge inputs in the source tree. Generate compiled scripts and LVLs outside it.

## Prose

Use plain, consistent language throughout documentation and messages. Write settings and help for players;
write API documentation for patch authors. Use established terms and describe behavior accurately.

Error messages should identify the failed operation and affected item, with useful details such as a path,
value or error code. Include the cause when it is known.

Keep setting descriptions to at most two short INI comment lines, including useful units and special values.
Use explicit `\n` line breaks; wrapping the C++ source does not wrap the generated description.

## Comments

- Document API contracts at their declarations, including ownership, lifetimes, units and thread requirements where relevant.
- Explain non-obvious algorithms, callback ordering and assumptions about the game beside the implementation.
- Put details about a field or operation beside that field or operation.
- Use `//` comments in C++ and `--` in Lua, usually one or two lines. Longer explanations are appropriate when the code needs them.
