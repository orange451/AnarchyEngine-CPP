import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

from tools.vegetation.mcp_client import McpError, Studio, copy_project


def line(seq, text, kind="print"):
    # Shape of one run_lua output line (src/ide/McpTools.cpp OutputLines).
    return {"seq": seq, "kind": kind, "text": text}


class Fake(BaseHTTPRequestHandler):
    calls = []

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        Fake.calls.append((self.headers.get("Authorization"), body))
        if self.headers.get("Authorization") != "Bearer tok":
            self.send_response(401)
            self.end_headers()
            return
        if body["method"] == "initialize":
            result = {"protocolVersion": "2025-03-26", "serverInfo": {"name": "fake"}}
        else:
            name, args = body["params"]["name"], body["params"]["arguments"]
            if name == "run_lua":
                bad = "error(" in args["source"]
                sc = {"output": [line(1, "boom", "error")] if bad else [line(1, "hi"), line(2, "FIX_DONE\t24")],
                      "error": bad}
                result = {"content": [{"type": "text", "text": json.dumps(sc)}], "structuredContent": sc,
                          "isError": False}
            elif name == "explode":
                result = {"content": [{"type": "text", "text": "it broke"}], "isError": True}
            else:
                result = {"content": [{"type": "text", "text": "plain"}], "isError": False}
        out = json.dumps({"jsonrpc": "2.0", "id": body["id"], "result": result}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *a):
        pass


@pytest.fixture
def server():
    srv = HTTPServer(("127.0.0.1", 0), Fake)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield srv.server_port
    srv.shutdown()


def test_initialize_and_lua(server):
    s = Studio(server, "tok")
    s.initialize()
    assert s.lua("print('hi')") == ["hi", "FIX_DONE\t24"]
    assert Fake.calls[-1][0] == "Bearer tok"
    assert Fake.calls[-1][1]["params"] == {"name": "run_lua", "arguments": {"source": "print('hi')"}}


def test_plain_tool_result(server):
    assert Studio(server, "tok").call("get_studio_info") == {"text": "plain"}


def test_tool_error_raises(server):
    s = Studio(server, "tok")
    with pytest.raises(McpError, match="explode.*it broke"):
        s.call("explode")
    with pytest.raises(McpError, match="run_lua.*boom"):
        s.lua("error('x')")


def test_wrong_token_raises(server):
    with pytest.raises(McpError, match="401"):
        Studio(server, "nope").initialize()


def test_wait_until_ready_times_out():
    s = Studio(1, "tok")   # nothing listens on port 1
    with pytest.raises(McpError, match="not answering"):
        s.wait_until_ready(timeout_s=0.5)


def test_copy_project(tmp_path):
    src = tmp_path / "A"
    (src / "src").mkdir(parents=True)
    (src / "project.json").write_text(json.dumps({"name": "A", "format": 1}))
    (src / "src" / "x.json").write_text("{}")
    copy_project(str(src), str(tmp_path / "B"), "B")
    assert json.load(open(tmp_path / "B" / "project.json")) == {"name": "B", "format": 1}
    assert (tmp_path / "B" / "src" / "x.json").exists()
    assert json.load(open(src / "project.json"))["name"] == "A"   # the original is untouched
    with pytest.raises(FileExistsError):
        copy_project(str(src), str(tmp_path / "B"), "B")
