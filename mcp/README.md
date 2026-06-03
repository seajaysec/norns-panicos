# maiden-mcp

A companion **MCP server** for a norns / maiden device (PanicOS port). It lets an
agent (or any MCP client) drive the device: run REPL commands, load/read/write
scripts, manage `dust` files, and check engine-name conflicts.

## Why it shares plumbing with the maiden UI (B2 ↔ B3)

maiden's install-time **engine deconfliction** dialog and this MCP server ask the
device the same two questions:

- `engines_list()` → which SuperCollider engines are registered (class → script)
- `engine_check_conflict(name)` → does this engine name collide, and what's a free name?

The web UI uses it to warn before copying a script; the MCP server exposes it as a
tool. Same device-bridge surface, two front-ends — which is why they were built
together.

## Tools

| tool | purpose |
|------|---------|
| `device_info()` | connection + basic device info |
| `repl(command, target="matron")` | send Lua to matron / SC to `sc`, return output |
| `script_list()` | list scripts in `dust/code` |
| `script_load(name)` | load & run a script |
| `file_list(path="dust")` | list files (dust is the top limit) |
| `file_read(path)` | read a file |
| `file_write(path, content)` | create / overwrite a file |
| `engines_list()` | registered engines (class → script) |
| `engine_check_conflict(name)` | collision check + free-name suggestion |

## Run

```bash
# verify the plumbing with an in-memory device — no hardware, no MCP install:
python3 maiden_mcp.py --selftest

# MCP server over stdio against an in-memory device:
python3 maiden_mcp.py --mock

# MCP server over stdio against a real device:
python3 maiden_mcp.py --host norns.local --api-port 5000 --ws-port 5555
```

- Real REPL over websocket needs `pip install websocket-client`.
- Running as an MCP server needs `pip install "mcp[cli]"`.
- `--api-port` / `--ws-port` must match your maiden build (PanicOS defaults may differ;
  maiden's REST API is `/api/v1`, matron's REPL is a websocket).

## Register with an MCP client

Example `claude_desktop_config.json` (or any MCP client) entry:

```json
{
  "mcpServers": {
    "maiden": {
      "command": "python3",
      "args": ["/Users/seajay/gits/norns-panicos/mcp/maiden_mcp.py", "--host", "norns.local"]
    }
  }
}
```

## Status

The bridge logic + tool schemas are complete and self-tested in mock mode. The
real-device REST/websocket calls are implemented per maiden's documented API but
are **untested on hardware** — verify ports against your PanicOS image.
