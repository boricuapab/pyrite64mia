# MCP integration (Codex and Claude)

Pyrite64 includes an optional C++ bridge for the independent
[Pyrite64 MCP server](https://github.com/boricuapab/pyrata64-mcp).
It allows Codex and Claude to inspect and edit the open scene, initialize native
components, select objects, save the project and reload assets.

The companion server supplies searchable Pyrite64 documentation/source,
offline project tools and CLI build/clean integration. Its default Streamable
HTTP endpoint is `http://127.0.0.1:8974/mcp`; stdio is also available.
Port 8974 belongs to that server. The editor bridge uses a local JSON mailbox,
with all commands executed on the editor's main thread.

## Start from the editor

Open a game project and choose **MCP > Start Server** in the top menu. On first
use, select the companion `pyrata64-mcp` repository folder using **Browse...**.
Set up that repository's `.venv` with its documented launcher/setup instructions
first. The default port is **8974**. **Save and Start** stores the selection in
local editor preferences and starts live MCP for the open project. No terminal
or environment-variable setup is needed for this workflow.

The menu shows Starting/Running/Stopped status and provides **Stop Server**,
**Server Settings**, **Server Log**, **Copy MCP URL** and commands for Codex and
Claude Code. Startup failures, including missing dependencies or an occupied
port, are visible in the log. The server process is owned by the editor and
stops on project close/change or editor exit. On Windows its process tree is
managed together, including any CLI build children. An externally launched
server is never stopped by this menu. No dependencies are downloaded by the editor.

Repository locations and port selections are saved only in local preferences.
Source files contain no user-specific installation or game paths.

## Enable the bridge

Build Pyrite64 normally from this checkout. The bridge is already integrated
through `src/pyrite64_mcp_bridge.h` and the frame-loop hook in `src/main.cpp`;
no extra linked library or installation patch is required.

The bridge is disabled by default. Before launching the editor, set
`PYRITE64_MCP_BRIDGE_DIR` to an absolute directory shared with the companion
server. For example, from the editor installation directory on Windows:

```powershell
$env:PYRITE64_MCP_BRIDGE_DIR = Join-Path (Get-Location) '.pyrite64-mcp/bridge'
.\pyrite64.exe 'C:\path\to\game\project.p64proj'
```

On Linux/macOS:

```sh
export PYRITE64_MCP_BRIDGE_DIR="$PWD/.pyrite64-mcp/bridge"
./pyrite64 /path/to/game/project.p64proj
```

Use your own game path in these examples. In the companion server's environment,
set the same `PYRITE64_MCP_BRIDGE_DIR`, or pass the same directory with
`--bridge-dir`. Start the server with `--mode live`, `--project` pointing at the
game directory, and `--executable` pointing at the rebuilt editor.
The companion repository includes installation instructions, client configuration
examples and launch scripts.

## Connect a client

Start the companion HTTP server, then register it with either client:

```sh
codex mcp add pyrite64 --url http://127.0.0.1:8974/mcp
claude mcp add --transport http pyrite64 http://127.0.0.1:8974/mcp
```

Use `get_status` to verify the editor connection. The companion repository also
provides a stdio configuration example for Claude Desktop.

## Supported live operations

- Read project configuration and the currently open scene.
- Create, update, delete and reparent objects through scene transactions.
- Add all 14 native component types using Pyrite64's real initialization defaults.
- Edit/remove components through validated scene transactions.
- Select objects, save the project and rescan assets.

Scene changes use compare-and-swap against editor memory, preserve quaternion
ordering and record native undo changes. Live edits remain unsaved until
`save_project` or a normal editor save. Requests verify the project path, reject
stale/expired transactions and refuse changes while a build/run is in progress.
If a request times out, inspect current scene state before retrying: the change
may already have executed.

Open the requested scene in the editor before issuing scene commands. Prefab
overrides should be authored through the editor; unpack an instance before direct
object/component edits. Arbitrary editor code execution and viewport screenshots are not provided by this bridge.

## Offline workflows and builds

The companion server can edit scene JSON, C++ scripts, graphs, prefabs and asset
configuration without the bridge, and invoke Pyrite64's documented CLI build or
clean command. Close the game project in the editor before offline file edits.
ROM builds still require an installed, working N64 toolchain.

The companion MCP tool `build_and_run` (or `build_project` with `run=true`) saves
live changes, builds the game, and launches its fresh ROM in Ares. Set the
project emulator path to Ares, provide `ares_path` in the tool call, configure
`--ares`/`PYRITE64_ARES`, or put Ares on `PATH`. Failed builds and missing, empty
or unchanged ROMs are not launched. The tool reports emulator startup separately
from build success. Restart the MCP server after updating the companion repository
to discover the new tool. When the server is managed by the Windows editor, Ares
is part of its process tree and closes when that server is stopped.


## Local files

Mailbox requests/responses belong in the configured local directory and are not
source assets. `.pyrite64-mcp/` directories, installer `*.mcp-backup` files,
executables and build output should remain untracked.
