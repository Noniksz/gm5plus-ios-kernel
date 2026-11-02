#!/usr/bin/env python3
"""AgentRouter Stream Fix Proxy for OpenCode"""

import http.server
import socketserver
import urllib.error
import urllib.request

PORT = 8318
TARGET = "https://agentrouter.org"


class ProxyHandler(http.server.BaseHTTPRequestHandler):

  def do_OPTIONS(self):
    self.send_response(200)
    self.send_header("Access-Control-Allow-Origin", "*")
    self.send_header(
        "Access-Control-Allow-Methods", "GET, POST, OPTIONS, PUT, DELETE"
    )
    self.send_header("Access-Control-Allow-Headers", "*")
    self.end_headers()

  def do_POST(self):
    url = f"{TARGET}{self.path}"
    content_len = int(self.headers.get("Content-Length", 0))
    body = self.rfile.read(content_len) if content_len > 0 else None

    # AgentRouter WAF'ını aşan User-Agent ve başlıklar
    headers = {}
    for k, v in self.headers.items():
      if k.lower() not in ["host", "content-length", "user-agent"]:
        headers[k] = v
    headers["Host"] = "agentrouter.org"
    headers["User-Agent"] = "opencode/1.19.0"

    req = urllib.request.Request(
        url, data=body, headers=headers, method="POST"
    )
    try:
      with urllib.request.urlopen(req) as resp:
        self.send_response(resp.status)
        self.send_header("Access-Control-Allow-Origin", "*")
        for k, v in resp.getheaders():
          if k.lower() not in [
              "transfer-encoding",
              "content-length",
              "access-control-allow-origin",
          ]:
            self.send_header(k, v)
        self.end_headers()

        # OpenCode'u kilitleyen billing ve null paketlerini süzen filtre
        for line in resp:
          line_str = line.decode("utf-8", errors="ignore")
          if "billing" in line_str or line_str.strip() == "data: null":
            continue
          self.wfile.write(line)
          self.wfile.flush()
    except urllib.error.HTTPError as e:
      self.send_response(e.code)
      self.send_header("Access-Control-Allow-Origin", "*")
      self.end_headers()
      self.wfile.write(e.read())
    except Exception as e:
      self.send_response(500)
      self.send_header("Access-Control-Allow-Origin", "*")
      self.end_headers()
      self.wfile.write(str(e).encode())

  def do_GET(self):
    url = f"{TARGET}{self.path}"
    headers = {"Host": "agentrouter.org", "User-Agent": "opencode/1.19.0"}
    for k, v in self.headers.items():
      if k.lower() not in ["host", "user-agent"]:
        headers[k] = v
    req = urllib.request.Request(url, headers=headers, method="GET")
    try:
      with urllib.request.urlopen(req) as resp:
        self.send_response(resp.status)
        self.send_header("Access-Control-Allow-Origin", "*")
        for k, v in resp.getheaders():
          self.send_header(k, v)
        self.end_headers()
        self.wfile.write(resp.read())
    except Exception as e:
      self.send_response(500)
      self.send_header("Access-Control-Allow-Origin", "*")
      self.end_headers()
      self.wfile.write(str(e).encode())

  def log_message(self, format, *args):
    pass


if __name__ == "__main__":
  socketserver.TCPServer.allow_reuse_address = True
  with socketserver.TCPServer(("127.0.0.1", PORT), ProxyHandler) as httpd:
    print(
        f"✓ AgentRouter köprüsü devrede: http://127.0.0.1:{PORT} -> {TARGET}"
    )
    httpd.serve_forever()
