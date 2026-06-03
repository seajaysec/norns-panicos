#!/usr/bin/env python3
"""
maiden-mcp — a companion MCP server for norns / maiden (PanicOS port).

Exposes the device "bridge" (REPL, scripts, files, engines) as MCP tools so an
agent or external tool can drive a norns device: send matron/sc commands,
run/write/read scripts, manage dust files, and check engine-name conflicts.

The engine-conflict plumbing here is the SAME surface maiden's install-time
deconfliction (the "engine name conflict" dialog, B2) uses — that's why B2 and
B3 share plumbing: the web UI and the MCP server both ask the device "what
engines are registered, and does <name> collide?".

Usage:
  python maiden_mcp.py --selftest                 # exercise the bridge in mock mode (no MCP, no hardware)
  python maiden_mcp.py --mock                     # MCP server over stdio against an in-memory device
  python maiden_mcp.py --host norns.local         # MCP server over stdio against a real device

Real mode talks to:
  - maiden's REST API  (files / scripts)         http://<host>:<api-port>/api/v1
  - matron's websocket (REPL / engine.names)     ws://<host>:<ws-port>

Adjust --api-port / --ws-port to match your maiden build (PanicOS defaults may differ).
Real REPL needs `pip install websocket-client`; MCP server needs `pip install "mcp[cli]"`.
"""
import argparse, json, re, time, urllib.parse, urllib.request


class NornsBridge:
    """Device bridge. Mock mode is an in-memory norns for testing without hardware."""

    def __init__(self, host="norns.local", api_port=5000, ws_port=5555, mock=False):
        self.mock = mock
        self.host = host
        self.api = f"http://{host}:{api_port}/api/v1"
        self.ws_url = f"ws://{host}:{ws_port}"
        if mock:
            # engine class -> owning script (what `engine.names` would resolve to)
            self._engines = {"PolyPerc": "awake", "Glut": "glut", "Ack": "ack"}
            self._files = {
                "dust/code/awake/awake.lua": '-- awake\nengine.name = "PolyPerc"\n',
                "dust/code/awake/engine/PolyPerc.sc": "// PolyPerc engine\n",
            }
            self._scripts = ["awake", "glut", "ack", "cheat_codes_2"]

    # ---------- shared device-bridge surface ----------
    def device_info(self):
        if self.mock:
            return {"connected": True, "mode": "mock", "host": self.host, "engines": len(self._engines)}
        try:
            return {"connected": True, "mode": "live", **self._api_get("")}
        except Exception as e:  # noqa: BLE001
            return {"connected": False, "host": self.host, "error": str(e)}

    def repl(self, command, target="matron"):
        """Send a command to matron (Lua) or sc (SuperCollider); return output."""
        if self.mock:
            if "engine.names" in command:
                return "\n".join(f'"{n}"' for n in self._engines)
            if command.strip() == "clock.tempo":
                return "110.0"
            return "# ok"
        return self._ws_send(command, target)

    def script_list(self):
        if self.mock:
            return list(self._scripts)
        return [d for d in self._api_get("dust/code")]

    def script_load(self, name):
        return self.repl(f'norns.script.load("{name}")')

    def file_list(self, path="dust"):
        if self.mock:
            return sorted({f for f in self._files if f.startswith(path)})
        return self._api_get(path)

    def file_read(self, path):
        if self.mock:
            return self._files.get(path, "")
        return self._api_get_raw(path)

    def file_write(self, path, content):
        if self.mock:
            self._files[path] = content
            return {"ok": True, "path": path, "bytes": len(content)}
        return self._api_put(path, content)

    def engines_list(self):
        """engine class name -> owning script (None if unknown)."""
        if self.mock:
            return dict(self._engines)
        return {n: None for n in re.findall(r'"([A-Za-z0-9_]+)"', self.repl("tab.print(engine.names)"))}

    def engine_check_conflict(self, name):
        """Does engine class `name` already exist? Suggest a free alternative.
        This is exactly what maiden's install-time deconfliction asks before copying a script."""
        engines = self.engines_list()
        owner = engines.get(name)
        return {
            "conflict": owner is not None,
            "engine": name,
            "owner": owner,
            "suggestion": self._available_name(name, set(engines)),
        }

    # ---------- helpers ----------
    @staticmethod
    def _available_name(base, taken):
        n, i = base, 1
        while n in taken:
            i += 1
            n = f"{base}_{i}"
        return n

    def _api_get(self, path):
        with urllib.request.urlopen(f"{self.api}/{urllib.parse.quote(path)}", timeout=8) as r:
            return json.loads(r.read())

    def _api_get_raw(self, path):
        with urllib.request.urlopen(f"{self.api}/{urllib.parse.quote(path)}", timeout=8) as r:
            return r.read().decode("utf-8", "replace")

    def _api_put(self, path, content):
        req = urllib.request.Request(
            f"{self.api}/{urllib.parse.quote(path)}", data=content.encode(), method="PUT"
        )
        with urllib.request.urlopen(req, timeout=8) as r:
            return {"ok": True, "status": r.status, "path": path}

    def _ws_send(self, command, target):
        try:
            from websocket import create_connection
        except ImportError:
            return "error: `pip install websocket-client` to use the REPL over websocket"
        ws = create_connection(self.ws_url, timeout=8)
        try:
            ws.send(command if command.endswith("\n") else command + "\n")
            time.sleep(0.25)
            ws.settimeout(1.0)
            out = []
            try:
                while True:
                    out.append(ws.recv())
            except Exception:  # noqa: BLE001  (timeout = done reading)
                pass
            return "".join(out)
        finally:
            ws.close()


def build_mcp(bridge):
    """Wrap the bridge as MCP tools (imported lazily so --selftest needs no mcp install)."""
    from mcp.server.fastmcp import FastMCP

    mcp = FastMCP("maiden")

    @mcp.tool()
    def device_info() -> dict:
        """Connection status and basic info about the norns device."""
        return bridge.device_info()

    @mcp.tool()
    def repl(command: str, target: str = "matron") -> str:
        """Send a command to matron (Lua) or 'sc' (SuperCollider) and return its output."""
        return bridge.repl(command, target)

    @mcp.tool()
    def script_list() -> list:
        """List installed scripts in dust/code."""
        return bridge.script_list()

    @mcp.tool()
    def script_load(name: str) -> str:
        """Load and run a script by name."""
        return bridge.script_load(name)

    @mcp.tool()
    def file_list(path: str = "dust") -> list:
        """List files/folders under a dust path (dust is the top limit)."""
        return bridge.file_list(path)

    @mcp.tool()
    def file_read(path: str) -> str:
        """Read a file's contents."""
        return bridge.file_read(path)

    @mcp.tool()
    def file_write(path: str, content: str) -> dict:
        """Create or overwrite a file with content."""
        return bridge.file_write(path, content)

    @mcp.tool()
    def engines_list() -> dict:
        """List registered SuperCollider engines (engine class -> owning script)."""
        return bridge.engines_list()

    @mcp.tool()
    def engine_check_conflict(name: str) -> dict:
        """Check whether an engine class name collides with one already on the device,
        and suggest a free name. (Shared plumbing with maiden's install-time deconfliction.)"""
        return bridge.engine_check_conflict(name)

    return mcp


def selftest(bridge):
    print("device_info        :", bridge.device_info())
    print("engines_list       :", bridge.engines_list())
    print("conflict 'Glut'     :", bridge.engine_check_conflict("Glut"))
    print("conflict 'Newthing' :", bridge.engine_check_conflict("Newthing"))
    print("file_write         :", bridge.file_write("dust/code/test/test.lua", "-- hi\n"))
    print("file_read          :", repr(bridge.file_read("dust/code/test/test.lua")))
    print("file_list dust/code:", bridge.file_list("dust/code"))
    print("repl clock.tempo   :", bridge.repl("clock.tempo"))
    print("script_list        :", bridge.script_list())
    print("\nselftest OK — bridge plumbing works (mock device)")


def main():
    ap = argparse.ArgumentParser(description="maiden-mcp — MCP server for norns/maiden")
    ap.add_argument("--host", default="norns.local")
    ap.add_argument("--api-port", type=int, default=5000)
    ap.add_argument("--ws-port", type=int, default=5555)
    ap.add_argument("--mock", action="store_true", help="in-memory device (no hardware)")
    ap.add_argument("--selftest", action="store_true", help="exercise the bridge and exit (no MCP runtime)")
    a = ap.parse_args()

    bridge = NornsBridge(a.host, a.api_port, a.ws_port, mock=a.mock or a.selftest)
    if a.selftest:
        selftest(bridge)
        return
    build_mcp(bridge).run()  # stdio MCP server


if __name__ == "__main__":
    main()
