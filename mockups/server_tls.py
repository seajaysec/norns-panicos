#!/usr/bin/env python3
"""Persistent LAN server for maiden mockups — HTTP (8780) + HTTPS (8444)."""
import http.server, socketserver, ssl, threading, os

os.chdir(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))

class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, max-age=0')
        super().end_headers()
    def log_message(self, *a):
        pass

socketserver.TCPServer.allow_reuse_address = True

def serve_http():
    with socketserver.ThreadingTCPServer(("0.0.0.0", 8780), H) as httpd:
        print("HTTP  on 0.0.0.0:8780", flush=True)
        httpd.serve_forever()

def serve_https():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(HERE, "certs/cert.pem"),
                        os.path.join(HERE, "certs/key.pem"))
    httpd = socketserver.ThreadingTCPServer(("0.0.0.0", 8444), H)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    print("HTTPS on 0.0.0.0:8444", flush=True)
    httpd.serve_forever()

t = threading.Thread(target=serve_http, daemon=True)
t.start()
serve_https()
