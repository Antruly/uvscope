#!/usr/bin/env python3
"""Small, dependency-free reverse-proxy benchmark for uvscope.

Starts an in-process origin, a throwaway uvscope daemon and a reverse proxy,
then measures sequential keep-alive latency and throughput for the origin and
the proxy. The numbers are a reference for relative comparisons on the same
machine, not a cross-machine guarantee.

Usage:
    python3 scripts/bench.py ./build/uvscope [--requests 500] [--isolated]
"""

import argparse
import http.client
import http.cookiejar
import http.server
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

BODY = b'{"ok":true,"bench":"uvscope"}'


class OriginHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    # Without this the header and body writes collide with delayed ACKs and
    # every keep-alive response picks up ~40 ms of latency.
    disable_nagle_algorithm = True

    def do_GET(self):
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(BODY)))
        self.end_headers()
        self.wfile.write(BODY)

    def log_message(self, *_args):
        pass


def wait_for_port(host, port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def wait_for_health(base, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(base + "/api/health", timeout=1):
                return True
        except Exception:
            time.sleep(0.1)
    return False


class Client:
    def __init__(self, base):
        self.base = base
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar))

    def call(self, method, path, payload=None, timeout=30):
        data = json.dumps(payload).encode() if payload is not None else None
        request = urllib.request.Request(
            self.base + path, data=data, method=method,
            headers={"content-type": "application/json"} if data else {})
        try:
            with self.opener.open(request, timeout=timeout) as response:
                return response.status, json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read() or b"{}")


def measure(host, port, path, requests, warmup):
    try:
        connection = http.client.HTTPConnection(host, port, timeout=15)
        for _ in range(warmup):
            connection.request("GET", path)
            response = connection.getresponse()
            response.read()
            if response.status != 200:
                raise RuntimeError("warmup status %s" % response.status)
        samples = []
        started = time.perf_counter()
        for _ in range(requests):
            begin = time.perf_counter()
            connection.request("GET", path)
            response = connection.getresponse()
            body = response.read()
            samples.append((time.perf_counter() - begin) * 1000.0)
            if response.status != 200 or body != BODY:
                raise RuntimeError("unexpected response: %s %r" %
                                   (response.status, body[:80]))
        elapsed = time.perf_counter() - started
        connection.close()
    except OSError as error:
        raise RuntimeError("could not measure %s:%s: %s" % (host, port, error))
    samples.sort()
    count = len(samples)
    return {
        "requests": count,
        "seconds": elapsed,
        "throughput": count / elapsed if elapsed else 0.0,
        "meanMs": sum(samples) / count,
        "p50Ms": samples[count // 2],
        "p95Ms": samples[min(count - 1, int(count * 0.95))],
        "maxMs": samples[-1],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default="./build/uvscope")
    parser.add_argument("--port", type=int, default=18830)
    parser.add_argument("--proxy-port", type=int, default=18831)
    parser.add_argument("--origin-port", type=int, default=18832)
    parser.add_argument("--requests", type=int, default=500)
    parser.add_argument("--warmup", type=int, default=50)
    parser.add_argument("--isolated", action="store_true",
                        help="run the reverse proxy in a child process")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    binary = os.path.abspath(args.binary)
    if not os.path.exists(binary):
        raise SystemExit("bench: binary not found: %s" % binary)

    origin = http.server.ThreadingHTTPServer(
        ("127.0.0.1", args.origin_port), OriginHandler)
    origin_thread = threading.Thread(target=origin.serve_forever, daemon=True)
    origin_thread.start()

    data_dir = tempfile.mkdtemp(prefix="uvscope-bench.")
    daemon_log = os.path.join(data_dir, "daemon.log")
    environment = dict(os.environ)
    environment["UVSCOPE_DATA_DIR"] = data_dir
    log_file = open(daemon_log, "wb")
    daemon = subprocess.Popen(
        [binary, "--host", "127.0.0.1", "--port", str(args.port)],
        stdout=log_file, stderr=subprocess.STDOUT, env=environment)
    base = "http://127.0.0.1:%d" % args.port

    try:
        if not wait_for_health(base):
            raise SystemExit("bench: daemon did not start\n" +
                             open(daemon_log, encoding="utf-8",
                                  errors="replace").read()[-2000:])
        client = Client(base)
        status, payload = client.call(
            "POST", "/api/setup/init",
            {"username": "bench-admin", "password": "bench-admin-password"})
        if status != 200:
            raise SystemExit("bench: setup failed: %s" % payload)

        status, payload = client.call("POST", "/api/proxies", {
            "name": "bench",
            "listenHost": "127.0.0.1",
            "listenPort": args.proxy_port,
            "target": "http://127.0.0.1:%d" % args.origin_port,
            "start": True,
            "isolated": args.isolated,
            "rules": [],
        })
        if status != 200 or not payload.get("proxies"):
            raise SystemExit("bench: proxy creation failed: %s" % payload)
        proxy_id = next(
            (item.get("id") for item in payload["proxies"]
             if item.get("name") == "bench"), None)
        if not proxy_id:
            raise SystemExit("bench: proxy id missing: %s" % payload)
        if not wait_for_port("127.0.0.1", args.proxy_port, timeout=15):
            raise SystemExit("bench: proxy did not listen\n" +
                             open(daemon_log, encoding="utf-8",
                                  errors="replace").read()[-2000:])

        direct = measure("127.0.0.1", args.origin_port, "/bench",
                         args.requests, args.warmup)
        proxied = measure("127.0.0.1", args.proxy_port, "/bench",
                          args.requests, args.warmup)

        status, payload = client.call("GET", "/api/health")
        version = payload.get("version", "") if isinstance(payload, dict) else ""
        mode = "isolated child" if args.isolated else "in-process"

        if args.json:
            print(json.dumps({
                "version": version,
                "platform": platform.platform(),
                "mode": mode,
                "direct": direct,
                "proxy": proxied,
            }, indent=2))
        else:
            print("# uvscope proxy benchmark")
            print()
            print("- version: %s" % (version or "unknown"))
            print("- platform: %s" % platform.platform())
            print("- proxy: %s" % mode)
            print("- requests: %d, warmup: %d, keep-alive, sequential"
                  % (args.requests, args.warmup))
            print()
            print("| Path | req/s | mean ms | p50 ms | p95 ms | max ms |")
            print("| --- | ---: | ---: | ---: | ---: | ---: |")
            for label, result in (("direct origin", direct),
                                  ("reverse proxy", proxied)):
                print("| %s | %.1f | %.2f | %.2f | %.2f | %.2f |" % (
                    label, result["throughput"], result["meanMs"],
                    result["p50Ms"], result["p95Ms"], result["maxMs"]))
            print()
            print("Compare runs on the same machine; absolute numbers include "
                  "the Python client.")
    finally:
        try:
            daemon.terminate()
            daemon.wait(timeout=10)
        except Exception:
            daemon.kill()
        log_file.close()
        origin.shutdown()
        origin.server_close()
        shutil.rmtree(data_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
