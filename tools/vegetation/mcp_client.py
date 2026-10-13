"""Minimal JSON-RPC client for AnarchyStudio's MCP server, plus launch and copy helpers.

Wire format (src/ide/McpServer.cpp): POST /mcp with a bearer token; replies are
plain JSON {"result": ...} or {"error": ...}. tools/call results carry content
text, an optional structuredContent object, and isError.
"""
import base64
import json
import os
import shutil
import subprocess
import time
import urllib.error
import urllib.request


class McpError(RuntimeError):
    pass


class Studio:
    def __init__(self, port: int, token: str, base: str = "http://127.0.0.1"):
        self.url = f"{base}:{port}/mcp"
        self.token = token
        self._id = 0

    def _rpc(self, method: str, params: dict, timeout: float = 900) -> dict:
        self._id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self._id, "method": method, "params": params}).encode()
        req = urllib.request.Request(self.url, data=body, method="POST", headers={
            "Authorization": f"Bearer {self.token}", "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                reply = json.loads(r.read())
        except urllib.error.HTTPError as e:
            raise McpError(f"{method}: HTTP {e.code}") from e
        if "error" in reply:
            raise McpError(f"{method}: {reply['error']}")
        return reply["result"]

    def initialize(self) -> dict:
        return self._rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                        "clientInfo": {"name": "vegetation-import", "version": "1"}})

    def wait_until_ready(self, timeout_s: float = 120) -> None:
        deadline = time.time() + timeout_s
        while True:
            try:
                self.initialize()
                return
            except (urllib.error.URLError, OSError, McpError):
                if time.time() > deadline:
                    raise McpError(f"studio not answering on {self.url} after {timeout_s}s")
                time.sleep(min(1.0, timeout_s / 4))

    def call(self, tool: str, **arguments) -> dict:
        result = self._rpc("tools/call", {"name": tool, "arguments": arguments})
        text = "".join(c.get("text", "") for c in result.get("content", []))
        if result.get("isError"):
            raise McpError(f"{tool}: {text}")
        return result.get("structuredContent") or {"text": text}

    def screenshot(self, max_size: int = 1600) -> bytes:
        """PNG bytes of the Scene View. The server moves the image into a content item."""
        result = self._rpc("tools/call", {"name": "screenshot", "arguments": {"max_size": max_size}})
        if result.get("isError"):
            raise McpError("screenshot: " + "".join(c.get("text", "") for c in result.get("content", [])))
        for item in result.get("content", []):
            if item.get("type") == "image":
                return base64.b64decode(item["data"])
        raise McpError("screenshot: no image in the result")

    def lua(self, source: str) -> list:
        """Runs Luau in the studio's console VM; returns the printed lines' text."""
        r = self.call("run_lua", source=source)
        lines = [e["text"] if isinstance(e, dict) else str(e) for e in r.get("output", [])]
        if r.get("error"):
            raise McpError("run_lua failed: " + "\n".join(lines[-40:]))
        return lines


def launch_studio(exe: str, project_dir: str, port: int, token: str) -> subprocess.Popen:
    env = dict(os.environ, ANARCHY_MCP_PORT=str(port), ANARCHY_MCP_TOKEN=token)
    return subprocess.Popen([exe, project_dir], env=env, cwd=os.path.dirname(exe))


def copy_project(src_dir: str, dst_dir: str, new_name: str) -> None:
    if os.path.exists(dst_dir):
        raise FileExistsError(dst_dir)
    shutil.copytree(src_dir, dst_dir)
    project_json = os.path.join(dst_dir, "project.json")
    with open(project_json) as f:
        data = json.load(f)
    data["name"] = new_name
    with open(project_json, "w", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
