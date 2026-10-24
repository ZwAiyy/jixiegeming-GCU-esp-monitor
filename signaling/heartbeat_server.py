#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GCU OLED — IP 心跳信令服务（部署在内网 Linux 服务器上）

作用：8266 板子和电脑端转发器（GcuCast）的 IP 都是 DHCP 动态分配的，
广播又被校园网路由挡住，两边互相找不到。这台服务器当"留言板"：
  · 板子每 30 秒 POST /heartbeat —— 服务器直接记录请求的来源 IP
  · GcuCast 每 5 秒 GET /ip —— 拿到板子的最新 IP，自动更新单播目标
真正的监控数据不走这里，仍走校园网 UDP 直连。

零第三方依赖，Python 3 自带库即可运行：
    python3 heartbeat_server.py
建议用 systemd 常驻（见 gcu-heartbeat.service）。

改端口和口令：改下面的 PORT / TOKEN，两端保持一致。
"""
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8300
TOKEN = "gcu-hud-2026"          # 两端一致的口令，防撞车

_state = {"board_ip": None, "ts": 0.0}
_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # 转发器取板子 IP：/ip?token=xxx
        if self.path.startswith("/ip"):
            if f"token={TOKEN}" not in self.path:
                self._json({"ok": False, "err": "bad token"}, 403)
                return
            with _lock:
                ip, ts = _state["board_ip"], _state["ts"]
            age = int(time.time() - ts) if ts else None
            self._json({"ok": ip is not None, "ip": ip, "age": age})
        else:
            self._json({"ok": False}, 404)

    def do_POST(self):
        # 板子心跳：/heartbeat?token=xxx（来源 IP 就是板子的 IP）
        if self.path.startswith("/heartbeat"):
            if f"token={TOKEN}" not in self.path:
                self._json({"ok": False, "err": "bad token"}, 403)
                return
            ip = self.client_address[0]
            with _lock:
                _state["board_ip"], _state["ts"] = ip, time.time()
            self._json({"ok": True, "ip": ip})
        else:
            self._json({"ok": False}, 404)

    def log_message(self, *args):   # 静默，不刷日志
        pass


if __name__ == "__main__":
    print(f"[heartbeat] listening on 0.0.0.0:{PORT}  token={TOKEN}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
