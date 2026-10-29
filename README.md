# dvv-base

Foundation library of the dvv stack used by rawreel: errors (`Result`, `DVV_TRY`, `co_try`),
factory-only creation with a lifetime registry and leak check, coroutine tasks and executors,
logging, a JSON wrapper, a URI parser and per-user paths. C++23, no dependencies beyond the standard
library and nlohmann/json.

`dvv::getEnvironment` and `dvv::setEnvironment` take and return UTF-8 names and values; child
processes see the values they set. Invalid UTF-8, empty names and names with `=` are rejected.

## Build

Builds and tests on Windows (Visual Studio 2026) and Linux (Clang 19 with libstdc++ 14) with
[dvv-build](https://github.com/BarabashkaD/dvv-build), which provides the presets, the shared CMake
modules, the third-party dependencies and the CI workflow; its README lists the prerequisites.
dvv-build is cloned next to dvv-base at the version of the `dvv-build` line in `dvv-deps.txt`:

```powershell
git clone https://github.com/BarabashkaD/dvv-base
git clone --branch v0.1.1 https://github.com/BarabashkaD/dvv-build
python -m pip install -r dvv-build/tools/requirements.txt
cmake -DREPO=dvv-base -P dvv-build/tools/fetch-deps.cmake
./dvv-build/bootstrap.ps1
cd dvv-base
cmake --workflow --preset vs2026
```

On Linux: `./dvv-build/bootstrap.sh`, then `cmake --workflow --preset ninja-clang`.
Applications use dvv-base as a git submodule in their `libs/` folder, next to dvv-build.

Licence: MIT.
