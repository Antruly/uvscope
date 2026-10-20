#!/usr/bin/env bash
# End-to-end smoke test used by CI and release verification.
set -euo pipefail

BIN="${1:-./build/uvscope}"
PORT="${2:-18787}"
PROXY_PORT="${3:-18788}"
TCP_PORT="${4:-18789}"
TCP_LISTEN_PORT="${5:-18790}"
UDP_LISTEN_PORT="${6:-18791}"
SOCKS5_PORT="${7:-18792}"
SOCKS5_AUTH_PORT="${8:-18793}"
SOCKS5_UDP_TARGET_PORT="${9:-18794}"
DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/uvscope-smoke.XXXXXX")"
COOKIE_JAR="${DATA_DIR}/cookies.txt"
LOG="${DATA_DIR}/daemon.log"
PID=""
ECHO_PID=""

cleanup() {
  if [[ -n "${ECHO_PID}" ]] && kill -0 "${ECHO_PID}" 2>/dev/null; then
    kill "${ECHO_PID}" 2>/dev/null || true
    wait "${ECHO_PID}" 2>/dev/null || true
  fi
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill "${PID}" 2>/dev/null || true
    for _ in $(seq 1 30); do
      kill -0 "${PID}" 2>/dev/null || break
      sleep 0.1
    done
    kill -9 "${PID}" 2>/dev/null || true
  fi
  rm -rf "${DATA_DIR}"
}
trap cleanup EXIT

UVSCOPE_DATA_DIR="${DATA_DIR}" "${BIN}" --host 127.0.0.1 --port "${PORT}" >"${LOG}" 2>&1 &
PID=$!

for _ in $(seq 1 80); do
  if curl -fsS --max-time 1 "http://127.0.0.1:${PORT}/api/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "${PID}" 2>/dev/null; then
    echo "daemon exited during startup" >&2
    cat "${LOG}" >&2
    exit 1
  fi
  sleep 0.1
done

# Local TCP echo server for the persistent-session assertions.
cat >"${DATA_DIR}/echo.py" <<'PY'
import socket
import sys
import threading

port = int(sys.argv[1])
udp_port = int(sys.argv[2]) if len(sys.argv) > 2 else 0
server = socket.socket()
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(("127.0.0.1", port))
server.listen(16)
print("LISTENING", flush=True)


def handle(conn):
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            conn.sendall(b"echo:" + data)
    except OSError:
        pass
    finally:
        conn.close()


if udp_port:
    udp_echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_echo.bind(("127.0.0.1", udp_port))

    def handle_udp():
        while True:
            data, peer = udp_echo.recvfrom(65536)
            udp_echo.sendto(b"udp:" + data, peer)

    threading.Thread(target=handle_udp, daemon=True).start()

while True:
    connection, _ = server.accept()
    threading.Thread(target=handle, args=(connection,), daemon=True).start()
PY
python3 "${DATA_DIR}/echo.py" "${TCP_PORT}" "${SOCKS5_UDP_TARGET_PORT}" >"${DATA_DIR}/echo.log" 2>&1 &
ECHO_PID=$!
for _ in $(seq 1 80); do
  if grep -q LISTENING "${DATA_DIR}/echo.log" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

curl -fsS "http://127.0.0.1:${PORT}/api/health" >/dev/null
curl -fsS -c "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/setup/init" \
  -H 'content-type: application/json' \
  -d '{"username":"ci-admin","password":"ci-admin-password"}' >/dev/null

curl -fsS -b "${COOKIE_JAR}" "http://127.0.0.1:${PORT}/api/dashboard" >/dev/null

HTTP_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/http/send" \
  -H 'content-type: application/json' \
  -d "{\"url\":\"http://127.0.0.1:${PORT}/api/health\",\"method\":\"GET\",\"protocol\":\"http1\",\"headers\":[],\"body\":\"\",\"timeoutMs\":5000}")"

PROXY_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/proxies" \
  -H 'content-type: application/json' \
  -d "{\"name\":\"smoke\",\"listenHost\":\"127.0.0.1\",\"listenPort\":${PROXY_PORT},\"target\":\"http://127.0.0.1:${PORT}\",\"start\":true}")"

sleep 0.3
PROXIED="$(curl -fsS --max-time 5 "http://127.0.0.1:${PROXY_PORT}/api/health")"

SESSION_RESULT="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/tcp/sessions" \
  -H 'content-type: application/json' \
  -d "{\"host\":\"127.0.0.1\",\"port\":${TCP_PORT},\"connectTimeoutMs\":3000}")"
SESSION_ID="$(printf '%s' "${SESSION_RESULT}" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("session",{}).get("id",""))')"
if [[ -z "${SESSION_ID}" ]]; then
  echo "could not create a TCP session: ${SESSION_RESULT}" >&2
  exit 1
fi
STATE=""
for _ in $(seq 1 50); do
  STATE="$(curl -fsS -b "${COOKIE_JAR}" "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("session",{}).get("state",""))')"
  [[ "${STATE}" == "connected" ]] && break
  sleep 0.1
done
if [[ "${STATE}" != "connected" ]]; then
  echo "TCP session did not connect (state=${STATE})" >&2
  exit 1
fi
SEND_ONE="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}/send" \
  -H 'content-type: application/json' -d '{"payload":"smoke-1","encoding":"text"}')"
SEND_TWO="$(curl -fsS -b "${COOKIE_JAR}" -X POST "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}/send" \
  -H 'content-type: application/json' -d '{"payload":"736d6f6b652d32","encoding":"hex"}')"
sleep 0.5
EVENTS_RESULT="$(curl -fsS -b "${COOKIE_JAR}" \
  "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}/events?after=0&limit=50")"
curl -fsS -b "${COOKIE_JAR}" -X DELETE "http://127.0.0.1:${PORT}/api/tcp/sessions/${SESSION_ID}" >/dev/null

HTTP_RESULT="${HTTP_RESULT}" PROXY_RESULT="${PROXY_RESULT}" PROXIED="${PROXIED}" \
SESSION_RESULT="${SESSION_RESULT}" SEND_ONE="${SEND_ONE}" SEND_TWO="${SEND_TWO}" \
EVENTS_RESULT="${EVENTS_RESULT}" python3 - <<'PY'
import json
import os
import sys

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


http = json.loads(os.environ["HTTP_RESULT"])
check(http.get("ok") is True, f"HTTP probe failed: {http.get('error')}")
check(http.get("status") == 200, f"HTTP probe status was {http.get('status')}")
check(http.get("streamId") == 0, f"HTTP/1.1 streamId should be 0: {http.get('streamId')}")
check(http.get("retryable") is False, f"HTTP/1.1 retryable should be false: {http.get('retryable')}")

proxy = json.loads(os.environ["PROXY_RESULT"])
check(proxy.get("ok") is True, f"proxy creation failed: {proxy.get('error')}")
check(any(p.get("running") for p in proxy.get("proxies", [])), "proxy is not running")

proxied = json.loads(os.environ["PROXIED"])
check(proxied.get("ok") is True, "proxied health endpoint failed")
check(proxied.get("name") == "uvscope", "proxied response was not uvscope")

session = json.loads(os.environ["SESSION_RESULT"])
check(session.get("ok") is True, f"TCP session creation failed: {session.get('error')}")
check(session.get("session", {}).get("state") in ("connecting", "connected"),
      "TCP session did not start")
check(json.loads(os.environ["SEND_ONE"]).get("ok") is True, "first TCP send was rejected")
check(json.loads(os.environ["SEND_TWO"]).get("ok") is True, "second TCP send was rejected")
events = json.loads(os.environ["EVENTS_RESULT"])
check(events.get("ok") is True, "TCP session events failed")
texts = [item.get("payload", {}).get("text") for item in events.get("events", [])
         if item.get("type") == "recv"]
check(texts[:2] == ["echo:smoke-1", "echo:smoke-2"],
      f"TCP session echo mismatch: {texts}")
check(events.get("session", {}).get("queuedWrites") == 0,
      "TCP session still reports queued writes")

if failures:
    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    sys.exit(1)
print("smoke test OK (setup, login, HTTP probe, reverse proxy, persistent TCP session)")
PY

BASE="http://127.0.0.1:${PORT}" COOKIE_JAR="${COOKIE_JAR}" \
TCP_LISTEN_PORT="${TCP_LISTEN_PORT}" UDP_LISTEN_PORT="${UDP_LISTEN_PORT}" \
SOCKS5_PORT="${SOCKS5_PORT}" SOCKS5_AUTH_PORT="${SOCKS5_AUTH_PORT}" \
SOCKS5_UDP_TARGET_PORT="${SOCKS5_UDP_TARGET_PORT}" TCP_PORT="${TCP_PORT}" python3 - <<'PY'
import http.cookiejar
import json
import os
import socket
import subprocess
import time
import urllib.error
import urllib.request

base = os.environ["BASE"]
jar = http.cookiejar.MozillaCookieJar(os.environ["COOKIE_JAR"])
jar.load(ignore_discard=True, ignore_expires=True)
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"content-type": "application/json"} if body is not None else {}
    request = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with opener.open(request, timeout=10) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def wait_listener(listener_id, state="listening", timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        status, payload = call("GET", f"/api/transport/listeners/{listener_id}")
        if payload.get("listener", {}).get("state") == state:
            return True
        time.sleep(0.1)
    return False


status, created = call("POST", "/api/transport/listeners", {
    "transport": "tcp", "name": "smoke-tcp", "host": "127.0.0.1",
    "port": int(os.environ["TCP_LISTEN_PORT"]), "start": True,
})
check(status == 200 and created.get("ok") is True, f"TCP listener create failed: {created}")
tcp_listener = created.get("listener")
check(bool(tcp_listener), "TCP listener id missing")
if tcp_listener:
    check(wait_listener(tcp_listener), "TCP listener did not start")
    client = socket.create_connection(("127.0.0.1", int(os.environ["TCP_LISTEN_PORT"])), timeout=5)
    client.sendall(b"listener-hello")
    session_id = ""
    for _ in range(50):
        status, sessions = call("GET", f"/api/transport/listeners/{tcp_listener}/sessions")
        if sessions.get("sessions"):
            session_id = sessions["sessions"][0]["id"]
            break
        time.sleep(0.1)
    check(bool(session_id), "TCP listener did not accept a session")
    if session_id:
        status, sent = call("POST", f"/api/transport/listeners/{tcp_listener}/send", {
            "sessionId": session_id, "payload": "listener-reply", "encoding": "text",
        })
        check(status == 200 and sent.get("ok") is True, f"listener send failed: {sent}")
        client.settimeout(5)
        reply = client.recv(4096)
        check(reply == b"listener-reply", f"TCP listener reply mismatch: {reply}")
        status, events = call(
            "GET", f"/api/transport/listeners/{tcp_listener}/sessions/{session_id}/events?after=0")
        texts = [item.get("payload", {}).get("text") for item in events.get("events", [])
                 if item.get("type") == "recv"]
        check("listener-hello" in texts, f"TCP listener did not record the client bytes: {texts}")
    client.close()
    status, deleted = call("DELETE", f"/api/transport/listeners/{tcp_listener}")
    check(status == 200 and deleted.get("ok") is True, f"TCP listener delete failed: {deleted}")

status, created = call("POST", "/api/transport/listeners", {
    "transport": "udp", "name": "smoke-udp", "host": "127.0.0.1",
    "port": int(os.environ["UDP_LISTEN_PORT"]), "start": True,
})
check(status == 200 and created.get("ok") is True, f"UDP listener create failed: {created}")
udp_listener = created.get("listener")
check(bool(udp_listener), "UDP listener id missing")
if udp_listener:
    check(wait_listener(udp_listener), "UDP listener did not start")
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 0))
    udp.settimeout(5)
    udp.sendto(b"udp-smoke", ("127.0.0.1", int(os.environ["UDP_LISTEN_PORT"])))
    peer = ""
    for _ in range(50):
        status, events = call(
            "GET", f"/api/transport/listeners/{udp_listener}/events?after=0&limit=50")
        datagrams = [item for item in events.get("events", []) if item.get("type") == "datagram"]
        if datagrams:
            peer = datagrams[0].get("peer", "")
            break
        time.sleep(0.1)
    check(bool(peer), "UDP listener did not receive the datagram")
    if peer:
        host, port_text = peer.rsplit(":", 1)
        status, sent = call("POST", f"/api/transport/listeners/{udp_listener}/send", {
            "peerHost": host, "peerPort": int(port_text),
            "payload": "udp-reply", "encoding": "text",
        })
        check(status == 200 and sent.get("ok") is True, f"UDP listener send failed: {sent}")
        reply, _ = udp.recvfrom(4096)
        check(reply == b"udp-reply", f"UDP listener reply mismatch: {reply}")
    udp.close()
    status, deleted = call("DELETE", f"/api/transport/listeners/{udp_listener}")
    check(status == 200 and deleted.get("ok") is True, f"UDP listener delete failed: {deleted}")

def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data += chunk
    return data


def socks5_connect(port, target_host, target_port, username=None, password=None):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    if username is None:
        sock.sendall(b"\x05\x01\x00")
        method = recv_exact(sock, 2)
        if method != b"\x05\x00":
            sock.close()
            raise RuntimeError(f"method selection failed: {method.hex()}")
    else:
        sock.sendall(b"\x05\x01\x02")
        method = recv_exact(sock, 2)
        if method != b"\x05\x02":
            sock.close()
            raise RuntimeError(f"auth method not selected: {method.hex()}")
        user = username.encode()
        pw = password.encode()
        sock.sendall(b"\x01" + bytes([len(user)]) + user + bytes([len(pw)]) + pw)
        status = recv_exact(sock, 2)
        if status[1] != 0:
            sock.close()
            raise RuntimeError(f"authentication rejected: {status.hex()}")
    host = target_host.encode()
    sock.sendall(b"\x05\x01\x00\x03" + bytes([len(host)]) + host +
                 target_port.to_bytes(2, "big"))
    reply = recv_exact(sock, 4)
    if reply[1] != 0:
        sock.close()
        raise RuntimeError(f"CONNECT rejected: {reply.hex()}")
    atyp = reply[3]
    if atyp == 1:
        recv_exact(sock, 4)
    elif atyp == 4:
        recv_exact(sock, 16)
    elif atyp == 3:
        recv_exact(sock, recv_exact(sock, 1)[0])
    else:
        sock.close()
        raise RuntimeError(f"unexpected address type: {atyp}")
    recv_exact(sock, 2)
    return sock


def read_reply(sock):
    head = recv_exact(sock, 4)
    atyp = head[3]
    if atyp == 1:
        host = socket.inet_ntoa(recv_exact(sock, 4))
    elif atyp == 4:
        host = socket.inet_ntop(socket.AF_INET6, recv_exact(sock, 16))
    elif atyp == 3:
        host = recv_exact(sock, recv_exact(sock, 1)[0]).decode()
    else:
        raise RuntimeError(f"unexpected address type: {atyp}")
    port = int.from_bytes(recv_exact(sock, 2), "big")
    return head[1], host, port


def socks5_greeting(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall(b"\x05\x01\x00")
    if recv_exact(sock, 2) != b"\x05\x00":
        sock.close()
        raise RuntimeError("no-auth was not selected")
    return sock


def create_socks5(port, body):
    status, created = call("POST", "/api/socks5/servers", dict(body, port=port, start=True))
    check(status == 200 and created.get("ok") is True, f"SOCKS5 create failed: {created}")
    return created.get("server", "")


def wait_socks5(server_id, timeout=5):
    end = time.time() + timeout
    while time.time() < end:
        status, payload = call("GET", f"/api/socks5/servers/{server_id}")
        if payload.get("server", {}).get("state") == "listening":
            return True
        time.sleep(0.1)
    return False


socks5_server = create_socks5(int(os.environ["SOCKS5_PORT"]), {"name": "smoke-socks5"})
if socks5_server:
    check(wait_socks5(socks5_server), "SOCKS5 server did not start")
    try:
        tunnel = socks5_connect(int(os.environ["SOCKS5_PORT"]), "127.0.0.1",
                                int(os.environ["TCP_PORT"]))
        tunnel.sendall(b"through-socks5")
        reply = recv_exact(tunnel, len(b"echo:through-socks5"))
        check(reply == b"echo:through-socks5", f"SOCKS5 tunnel reply mismatch: {reply}")
        tunnel.close()
    except (OSError, RuntimeError, EOFError) as error:
        check(False, f"SOCKS5 tunnel failed: {error}")
    status, sessions = call("GET", f"/api/socks5/servers/{socks5_server}/sessions")
    established = [item for item in sessions.get("sessions", [])
                   if item.get("state") == "established"]
    check(bool(established), "SOCKS5 server did not record the established session")
    curl = subprocess.run(
        ["curl", "-fsS", "--max-time", "5", "--socks5-hostname",
         f"127.0.0.1:{os.environ['SOCKS5_PORT']}", f"{base}/api/health"],
        capture_output=True)
    check(curl.returncode == 0,
          f"curl --socks5-hostname failed: {curl.stderr.decode(errors='replace')}")

    # Outbound client paths must use the proxy and never fall back to direct.
    proxy = {"host": "127.0.0.1", "port": int(os.environ["SOCKS5_PORT"])}
    status, probed = call("POST", "/api/tcp/probe", {
        "host": "127.0.0.1", "port": int(os.environ["TCP_PORT"]),
        "payload": "client-socks5", "connectTimeoutMs": 3000,
        "readTimeoutMs": 2000, "proxy": proxy,
    })
    check(status == 200 and probed.get("ok") is True,
          f"TCP probe through SOCKS5 failed: {probed}")
    if probed.get("ok"):
        check(probed.get("received", {}).get("text") == "echo:client-socks5",
              f"TCP probe through SOCKS5 got {probed.get('received')}")

    status, http_probe = call("POST", "/api/http/send", {
        "url": f"{base}/api/health", "method": "GET", "protocol": "http1",
        "timeoutMs": 5000, "proxy": proxy,
    })
    check(status == 200 and http_probe.get("ok") is True and http_probe.get("status") == 200,
          f"HTTP probe through SOCKS5 failed: {http_probe}")

    status, udp_probe = call("POST", "/api/udp/probe", {
        "host": "127.0.0.1", "port": int(os.environ["SOCKS5_UDP_TARGET_PORT"]),
        "payload": "udp-client-socks5", "timeoutMs": 3000, "proxy": proxy,
    })
    check(status == 200 and udp_probe.get("ok") is True,
          f"UDP probe through SOCKS5 failed: {udp_probe}")
    if udp_probe.get("ok"):
        check(udp_probe.get("received", {}).get("text") == "udp:udp-client-socks5",
              f"UDP probe through SOCKS5 got {udp_probe.get('received')}")

    # BIND: the server listens, a peer connects, and the control connection
    # becomes the data tunnel after the second reply.
    try:
        ctrl = socks5_greeting(int(os.environ["SOCKS5_PORT"]))
        ctrl.sendall(b"\x05\x02\x00\x01" + socket.inet_aton("127.0.0.1") +
                     (0).to_bytes(2, "big"))
        rep, bind_host, bind_port = read_reply(ctrl)
        check(rep == 0, f"BIND request was rejected: {rep}")
        if rep == 0:
            peer = socket.create_connection((bind_host, bind_port), timeout=5)
            rep2, peer_host, peer_port = read_reply(ctrl)
            check(rep2 == 0, f"BIND peer reply was rejected: {rep2}")
            peer.sendall(b"bind-hello")
            echoed = recv_exact(ctrl, len(b"bind-hello"))
            check(echoed == b"bind-hello", f"BIND inbound mismatch: {echoed}")
            ctrl.sendall(b"bind-reply")
            echoed = recv_exact(peer, len(b"bind-reply"))
            check(echoed == b"bind-reply", f"BIND outbound mismatch: {echoed}")
            peer.close()
        ctrl.close()
    except (OSError, RuntimeError, EOFError) as error:
        check(False, f"BIND round-trip failed: {error}")

    # An unsupported command must be rejected, not silently accepted.
    try:
        ctrl = socks5_greeting(int(os.environ["SOCKS5_PORT"]))
        ctrl.sendall(b"\x05\x04\x00\x01" + socket.inet_aton("127.0.0.1") +
                     (80).to_bytes(2, "big"))
        rep, _, _ = read_reply(ctrl)
        check(rep == 7, f"unsupported command returned REP={rep}, expected 7")
        ctrl.close()
    except (OSError, RuntimeError, EOFError) as error:
        check(False, f"unsupported command probe failed: {error}")

    status, unreachable = call("POST", "/api/tcp/probe", {
        "host": "127.0.0.1", "port": int(os.environ["TCP_PORT"]),
        "payload": "x", "connectTimeoutMs": 2000,
        "proxy": {"host": "127.0.0.1", "port": int(os.environ["SOCKS5_PORT"]) + 50},
    })
    check(status == 200 and unreachable.get("ok") is False,
          f"a dead proxy fell back to a direct connection: {unreachable}")
    check(unreachable.get("proxy", {}).get("errorClass") == "proxy_unreachable",
          f"dead proxy was not classified: {unreachable.get('proxy')}")

    close_port = int(os.environ["SOCKS5_UDP_TARGET_PORT"]) + 50
    status, refused = call("POST", "/api/tcp/probe", {
        "host": "127.0.0.1", "port": close_port, "payload": "x",
        "connectTimeoutMs": 3000, "proxy": proxy,
    })
    check(status == 200 and refused.get("ok") is False,
          f"target refusal unexpectedly succeeded: {refused}")
    check(refused.get("proxy", {}).get("errorClass") == "target_rejected",
          f"target refusal was not classified: {refused.get('proxy')}")

    status, deleted = call("DELETE", f"/api/socks5/servers/{socks5_server}")
    check(status == 200 and deleted.get("ok") is True, f"SOCKS5 delete failed: {deleted}")

auth_server = create_socks5(int(os.environ["SOCKS5_AUTH_PORT"]),
                            {"name": "smoke-socks5-auth", "username": "smoke",
                             "password": "smoke-pass"})
if auth_server:
    check(wait_socks5(auth_server), "authenticated SOCKS5 server did not start")
    status, rejected = call("POST", "/api/tcp/probe", {
        "host": "127.0.0.1", "port": int(os.environ["TCP_PORT"]),
        "payload": "x", "connectTimeoutMs": 3000,
        "proxy": {"host": "127.0.0.1", "port": int(os.environ["SOCKS5_AUTH_PORT"]),
                  "username": "smoke", "password": "wrong"},
    })
    check(status == 200 and rejected.get("ok") is False,
          f"wrong proxy password unexpectedly connected: {rejected}")
    check(rejected.get("proxy", {}).get("errorClass") == "auth_failed",
          f"wrong proxy password was not classified: {rejected.get('proxy')}")
    try:
        socks5_connect(int(os.environ["SOCKS5_AUTH_PORT"]), "127.0.0.1",
                       int(os.environ["TCP_PORT"]), "smoke", "wrong")
        check(False, "SOCKS5 accepted a wrong password")
    except RuntimeError as error:
        check("rejected" in str(error), f"unexpected auth failure: {error}")
    try:
        tunnel = socks5_connect(int(os.environ["SOCKS5_AUTH_PORT"]), "127.0.0.1",
                                int(os.environ["TCP_PORT"]), "smoke", "smoke-pass")
        tunnel.sendall(b"auth-socks5")
        reply = recv_exact(tunnel, len(b"echo:auth-socks5"))
        check(reply == b"echo:auth-socks5", f"authenticated tunnel mismatch: {reply}")
        tunnel.close()
    except (OSError, RuntimeError, EOFError) as error:
        check(False, f"authenticated SOCKS5 tunnel failed: {error}")
    status, deleted = call("DELETE", f"/api/socks5/servers/{auth_server}")
    check(status == 200 and deleted.get("ok") is True, f"SOCKS5 auth delete failed: {deleted}")

# Preview documents: safe mode keeps scripts off, scripts mode opts in, both
# carry a CSP sandbox and a base URL injection.
for mode, csp_must, csp_must_not in [
    ("safe", "script-src 'none'", "allow-scripts"),
    ("scripts", "sandbox allow-scripts", None),
]:
    status, preview = call("POST", "/api/preview", {
        "html": "<!doctype html><html><head></head><body><div id=\"p\">ok</div></body></html>",
        "baseUrl": base + "/target/page",
        "mode": mode,
    })
    check(status == 200 and bool(preview.get("url")),
          f"preview {mode} create failed: {preview}")
    if preview.get("url"):
        with urllib.request.urlopen(base + preview["url"], timeout=5) as response:
            csp = response.headers.get("content-security-policy", "")
            page = response.read().decode()
        check(csp_must in csp, f"preview {mode} CSP missing {csp_must}: {csp}")
        if csp_must_not:
            check(csp_must_not not in csp,
                  f"preview {mode} CSP unexpectedly has {csp_must_not}: {csp}")
        check(f'<base href="{base}/target/page">' in page,
              f"preview {mode} did not inject the base URL")
        status, deleted = call("DELETE", f"/api/preview/{preview['id']}")
        check(status == 200 and deleted.get("ok") is True,
              f"preview {mode} delete failed: {deleted}")

if failures:
    for failure in failures:
        print("FAIL:", failure, file=sys.stderr)
    sys.exit(1)
print("smoke test OK (listener TCP/UDP, SOCKS5, outbound proxy, preview modes)")
PY
