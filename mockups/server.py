#!/usr/bin/env python3
import http.server, socketserver, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
PORT=8770
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control','no-store, max-age=0')
        super().end_headers()
    def log_message(self,*a): pass
socketserver.TCPServer.allow_reuse_address=True
with socketserver.TCPServer(("0.0.0.0",PORT),H) as httpd:
    print(f"serving on 0.0.0.0:{PORT}")
    httpd.serve_forever()
