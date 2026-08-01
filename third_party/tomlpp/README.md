# toml++ (vendored)

- Upstream: https://github.com/marzer/tomlplusplus
- Version: v3.4.0
- Commit: `30172438cee64926dc41fdd9c11fb3ba5b2ba9de`
- License: MIT (`LICENSE`)
- Why: `gba_recompile` parses game/BIOS TOML. Setup-host rebuilds must
  configure offline without FetchContent/git.

Only the amalgamated `toml.hpp` is committed (plus LICENSE). CMake looks
for `third_party/tomlpp/toml.hpp` before falling back to FetchContent.
