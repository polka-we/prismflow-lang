#!/bin/sh
# Проверка http.* на локальном сервере (нужны python3 и curl; иначе пропускается). Запуск: ./tests/http_test.sh [./prism]
cd "$(dirname "$0")/.." || exit 1
BIN=${1:-./prism}
command -v python3 >/dev/null && command -v curl >/dev/null || { echo "пропущено: нужны python3 и curl"; exit 0; }
cat > /tmp/pf_srv.$$.py <<'PY'
import http.server, json
class H(http.server.BaseHTTPRequestHandler):
    def _go(self):
        n = int(self.headers.get('Content-Length') or 0); b = self.rfile.read(n).decode()
        if self.path == '/404': self.send_response(404); self.end_headers(); self.wfile.write(b'nope'); return
        out = json.dumps({"method": self.command, "path": self.path, "body": b, "auth": self.headers.get('X-Auth')}).encode()
        self.send_response(200); self.send_header('X-Test', 'Hello'); self.end_headers(); self.wfile.write(out)
    do_GET = do_POST = do_PUT = do_DELETE = _go
    def log_message(self, *a): pass
http.server.HTTPServer(('127.0.0.1', 8765), H).serve_forever()
PY
python3 /tmp/pf_srv.$$.py & SP=$!; sleep 1
cat > /tmp/pf_t.$$.pr <<'PR'
u = "http://127.0.0.1:8765"
r = http.get(u + "/a?x=1")
print(r.status, r.ok, r.headers.x_test, r.body.json().path)
p = http.post(u + "/p", json_str([n: 1]), [x_auth: "tok"])
print(p.body.json().body, p.body.json().auth)
print(http.put(u + "/q", "zz").body.json().method, http.delete(u + "/q").body.json().method)
print(http.request([method: "POST", url: u + "/r", body: "hi", headers: [x_auth: "k"], timeout: 2000]).body.json().auth)
print(http.get(u + "/404"))
print(http.get("ftp://x"))
PR
"$BIN" --http /tmp/pf_t.$$.pr > /tmp/pf_o.$$ 2>&1
kill $SP 2>/dev/null
cat > /tmp/pf_e.$$ <<'EX'
200 true Hello /a?x=1
{"n":1} tok
PUT DELETE
k
error("http.get: HTTP 404")
error("http.get: url must start with http:// or https://")
EX
if cmp -s /tmp/pf_o.$$ /tmp/pf_e.$$; then echo "ok   http"; rc=0; else echo "FAIL http"; diff /tmp/pf_o.$$ /tmp/pf_e.$$; rc=1; fi
rm -f /tmp/pf_*.$$*; exit $rc
