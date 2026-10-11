# Persistent REA MCP client: keeps one `rea --mcp` (and its Ghidra session) alive; serves calls on a unix socket.
import json, os, socket, subprocess, sys, threading
SOCK = sys.argv[1]
p = subprocess.Popen(["rea", "--mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(SOCK + ".stderr", "w"), text=True, bufsize=1)
lock = threading.Lock(); nid = [0]
def rpc(method, params=None, notify=False):
    msg = {"jsonrpc": "2.0", "method": method}
    if params is not None: msg["params"] = params
    if not notify:
        nid[0] += 1; msg["id"] = nid[0]
    p.stdin.write(json.dumps(msg) + "\n"); p.stdin.flush()
    if notify: return None
    while True:
        line = p.stdout.readline()
        if not line: raise SystemExit("rea exited")
        r = json.loads(line)
        if r.get("id") == msg["id"]: return r
init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "reamcpd", "version": "1"}})
rpc("notifications/initialized", notify=True)
try: os.unlink(SOCK)
except FileNotFoundError: pass
s = socket.socket(socket.AF_UNIX); s.bind(SOCK); s.listen(4)
print("ready", flush=True)
while True:
    c, _ = s.accept()
    data = b""
    while not data.endswith(b"\n"):
        b = c.recv(65536)
        if not b: break
        data += b
    req = json.loads(data)
    with lock:
        if req.get("list"): r = rpc("tools/list", {})
        else: r = rpc("tools/call", {"name": req["tool"], "arguments": req.get("args", {})})
    c.sendall(json.dumps(r).encode() + b"\n"); c.close()
