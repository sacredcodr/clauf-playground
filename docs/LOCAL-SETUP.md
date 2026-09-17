# Local setup

## Prerequisites

The current tested setup is Windows x64, Visual Studio 2026 Community with the C++ desktop workload, CMake with the Visual Studio 18 2026 generator, Python 3 on Windows, and Ubuntu 24.04 under WSL. Keep the checkout on a Windows drive mounted under `/mnt` in WSL. UNC paths are not supported.

The runner and build helper currently select the distribution named `Ubuntu` explicitly. Check installed names with `wsl --list --verbose`.

Install these packages inside Ubuntu:

```sh
sudo apt update
sudo apt install git python3 cmake ninja-build clang pkg-config libffi-dev
```

Clang 18.1.3 and CMake 3.28.3 were used for the engine. GCC is not supported by the pinned lauf backend. The build downloads dependencies, so the first build needs network access.

## Build

From the project root:

```text
python build.py
cmake --preset windows
cmake --build --preset windows
```

The Python helper fetches the pinned clauf source, builds it in WSL and runs upstream tests. CMake builds the Windows frontend using `CMakePresets.json`. The app is at `out/build/windows/Release/sacred-playground.exe`. Keep it in the project tree so it can find the engine and examples.

Alternatively, open the folder in Visual Studio, select the `windows` configure preset, and build `sacred-playground` in Release. The engine still needs `python build.py` first.

The engine build uses `-include cstdarg` and `-Wno-error=format` for compatibility with the archived source and tested Clang version. An existing upstream checkout at a different revision is rejected rather than overwritten.

Close the app before rebuilding the same executable. This project is for tinkering; executable downloads are not planned.

## Editor and shortcuts

The source editor colours C tokens and displays line numbers. Colouring is lexical; it does not validate a snippet or indicate clauf support for a construct. Run is the outlined button at the top right; its shortcut is shown alongside it.

| Shortcut | Action |
| --- | --- |
| Ctrl+Enter | Run |
| Ctrl+O | Open a C file |
| Ctrl+S | Save to the current file; choose a path for a new snippet |
| Ctrl+Shift+S | Save as |
| Ctrl+Tab / Ctrl+Shift+Tab | Move keyboard focus between controls |

Go to line selects the first reported source line. Editing disables the action until the source matches the result again. AST and Bytecode inspect the snippet without executing `main`. The last session is saved separately in `.state/last.c` on Run, inspection and normal close.

## Limits

Snippets can be up to 32 KiB. Long-running programs are stopped, memory use is limited, and output is capped at 64 KiB. Interactive input is not supported.

Use trusted local code only; these limits do not make the app a sandbox.
