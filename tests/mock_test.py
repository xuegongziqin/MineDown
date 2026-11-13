#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MineDown 本地端到端测试：内置 mock 下载源 + 场景化断言。

用法:
    python tests/mock_test.py            # 跑全部场景
    python tests/mock_test.py chunk skip # 只跑名字里含这些词的场景

mock 服务器模拟 `custom` 镜像（HostOnly 布局：只换域名、路径不变），
因此 MineDown 用 `--mirror custom --custom-base http://127.0.0.1:<port>` 即可指向它。
"""

import hashlib
import json
import os
import random
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
EXE = os.path.join(PROJECT, "build", "MineDown.exe")
MINGW = r"D:\msys64\mingw64\bin"
VERSION = "1.0-test"

CLIENT_BYTES = 24 * 1024 * 1024
BIG_ASSET_BYTES = [12 * 1024 * 1024, 9 * 1024 * 1024, 6 * 1024 * 1024]
SMALL_ASSET_COUNT = 120
SMALL_ASSET_MAX = 32 * 1024


def sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


# ---------------------------------------------------------------- 数据集


def build_dataset():
    """构造一套自洽的 Minecraft 目录：清单、版本 JSON、资源索引、各类文件。"""
    rng = random.Random(20261112)
    files = {}          # 服务器路径 -> 字节
    meta = {"assets": [], "client": {}, "libraries": [], "log": {}, "big": []}

    # 客户端 jar（24 MiB，必触发分块）
    client = rng.randbytes(CLIENT_BYTES)
    ch = sha1(client)
    files[f"/v1/objects/{ch}/client.jar"] = client
    meta["client"] = {"sha1": ch, "size": len(client), "path": f"/v1/objects/{ch}/client.jar"}

    # 日志配置（很小的 xml）
    logxml = b'<?xml version="1.0" encoding="UTF-8"?><Configuration status="WARN"/>'
    lh = sha1(logxml)
    files[f"/v1/objects/{lh}/client-1.2.xml"] = logxml
    meta["log"] = {"sha1": lh, "size": len(logxml)}

    # 依赖库：4 个普通 jar + 1 个 natives-windows 分类器
    libraries = []
    lib_defs = [
        ("org.example", "core", "1.0", None, 400 * 1024),
        ("org.example", "util", "1.2", None, 150 * 1024),
        ("com.other", "json", "2.1", None, 90 * 1024),
        ("com.other", "audio", "0.9", None, 800 * 1024),
        ("org.example", "platform", "1.0", "natives-windows", 60 * 1024),
    ]
    for group, artifact, ver, classifier, size in lib_defs:
        data = rng.randbytes(size)
        h = sha1(data)
        base = f"{group.replace('.', '/')}/{artifact}/{ver}/{artifact}-{ver}"
        if classifier:
            rel = f"{base}-{classifier}.jar"
        else:
            rel = f"{base}.jar"
        files["/" + rel] = data
        libraries.append({
            "group": group, "artifact": artifact, "ver": ver,
            "classifier": classifier, "rel": rel, "sha1": h, "size": size,
        })
    meta["libraries"] = libraries

    # 资源对象：若干小文件 + 3 个大文件
    objects = {}
    for i in range(SMALL_ASSET_COUNT):
        size = rng.randint(4096, SMALL_ASSET_MAX)
        data = rng.randbytes(size)
        h = sha1(data)
        files[f"/{h[:2]}/{h}"] = data
        logical = f"minecraft/sounds/sfx/test{i:03d}.ogg"
        objects[logical] = {"hash": h, "size": size}
        meta["assets"].append({"hash": h, "size": size})
    for i, size in enumerate(BIG_ASSET_BYTES):
        data = rng.randbytes(size)
        h = sha1(data)
        files[f"/{h[:2]}/{h}"] = data
        logical = f"minecraft/sounds/music/game/big{i}.ogg"
        objects[logical] = {"hash": h, "size": size}
        meta["assets"].append({"hash": h, "size": size})
        meta["big"].append({"hash": h, "size": size, "path": f"/{h[:2]}/{h}"})

    index = {"objects": objects, "virtual": False}
    index_bytes = json.dumps(index).encode()
    ih = sha1(index_bytes)
    files[f"/v1/packages/{ih}/{VERSION}-assets.json"] = index_bytes

    total_asset = sum(o["size"] for o in objects.values())

    # 版本 JSON：内部一律写官方绝对地址，由 MineDown 的 applyMirror 改写
    version = {
        "id": VERSION,
        "type": "release",
        "mainClass": "net.minecraft.client.main.Main",
        "assets": VERSION,
        "downloads": {
            "client": {
                "url": f"https://piston-data.mojang.com/v1/objects/{ch}/client.jar",
                "sha1": ch, "size": len(client),
            }
        },
        "assetIndex": {
            "id": VERSION,
            "url": f"https://piston-meta.mojang.com/v1/packages/{ih}/{VERSION}-assets.json",
            "sha1": ih, "size": len(index_bytes), "totalSize": total_asset,
        },
        "logging": {
            "client": {
                "argument": "-Dlog4j.configurationFile=${path}",
                "file": {
                    "id": "client-1.2.xml",
                    "url": f"https://piston-data.mojang.com/v1/objects/{lh}/client-1.2.xml",
                    "sha1": lh, "size": len(logxml),
                },
            }
        },
        "javaVersion": {"majorVersion": 21, "component": "java-runtime-delta"},
        "libraries": [],
    }
    for lib in libraries:
        entry = {"name": f"{lib['group']}:{lib['artifact']}:{lib['ver']}"}
        art = {
            "path": lib["rel"],
            "url": f"https://libraries.minecraft.net/{lib['rel']}",
            "sha1": lib["sha1"], "size": lib["size"],
        }
        if lib["classifier"]:
            entry["name"] += f":{lib['classifier']}"
            entry["natives"] = {"windows": lib["classifier"]}
            entry["downloads"] = {"classifiers": {lib["classifier"]: art}}
        else:
            entry["downloads"] = {"artifact": art}
        version["libraries"].append(entry)
    version_bytes = json.dumps(version).encode()
    vh = sha1(version_bytes)
    files[f"/v1/packages/{vh}/{VERSION}.json"] = version_bytes

    manifest = {
        "latest": {"release": VERSION, "snapshot": VERSION},
        "versions": [{
            "id": VERSION, "type": "release",
            "url": f"https://piston-meta.mojang.com/v1/packages/{vh}/{VERSION}.json",
            "sha1": vh, "size": len(version_bytes),
            "releaseTime": "2026-01-01T00:00:00+00:00",
        }],
    }
    files["/mc/game/version_manifest_v2.json"] = json.dumps(manifest).encode()

    meta["total_files"] = 1 + 1 + 1 + len(libraries) + len(objects)  # client+log+index 另算
    return files, meta


# ---------------------------------------------------------------- mock 服务器


class State:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lock = threading.Lock()
        self.asset_hits = 0
        self.dropped = set()
        self.served = 0


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.serve(head=False)

    def do_HEAD(self):
        self.serve(head=True)

    def serve(self, head):
        cfg = self.server.state.cfg
        path = self.path.split("?")[0]

        data = cfg["files"].get(path)
        if data is None:
            body = b"not found"
            self.send_response(404)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head:
                self.wfile.write(body)
            return

        if not head:
            with self.server.state.lock:
                self.server.state.served += 1

        # 429 注入：只针对资源对象，前 N 个请求各拒一次
        if cfg.get("fail_first") and path in cfg.get("asset_paths", ()):
            with self.server.state.lock:
                take = self.server.state.asset_hits < cfg["fail_first"]
                if take:
                    self.server.state.asset_hits += 1
            if take:
                body = b"Too Many Requests"
                self.send_response(429)
                self.send_header("Retry-After", "1")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if not head:
                    self.wfile.write(body)
                return

        total = len(data)
        status = 200
        start, end = 0, total - 1
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            spec = rng[len("bytes="):].split(",")[0].strip()
            a, _, b = spec.partition("-")
            try:
                start = int(a) if a else 0
                end = int(b) if b and b.isdigit() else total - 1
            except ValueError:
                start, end = 0, total - 1
            if start >= total:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{total}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            end = min(end, total - 1)
            status = 206

        body = data[start:end + 1]

        # 损坏注入：把第一个字节翻掉，模拟镜像上的坏文件
        if cfg.get("corrupt") and cfg["corrupt"] in path and body:
            body = bytes([body[0] ^ 0xFF]) + body[1:]

        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(len(body)))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
        self.end_headers()
        if head:
            return

        # 模拟传输中断：每个路径只断一次，用来验证断点续传/重试
        drop_after = cfg.get("drop_after")
        if drop_after and cfg.get("drop_match", "") in path:
            with self.server.state.lock:
                first = path not in self.server.state.dropped
                if first:
                    self.server.state.dropped.add(path)
            if first:
                partial = body[:drop_after]
                self.write_limited(partial, cfg, None)
                try:
                    self.wfile.flush()
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                self.close_connection = True
                return

        self.write_limited(body, cfg, None)

    def write_limited(self, body, cfg, _unused):
        rate = cfg.get("rate")  # 每连接字节/秒
        chunk = 64 * 1024
        sent = 0
        t0 = time.time()
        while sent < len(body):
            piece = body[sent:sent + chunk]
            try:
                self.wfile.write(piece)
            except OSError:
                # 客户端主动断开（取消/暂停/分块被打断）是测试里的正常现象
                self.close_connection = True
                return
            sent += len(piece)
            if rate:
                target = sent / rate
                now = time.time() - t0
                if target > now:
                    time.sleep(target - now)


class QuietServer(ThreadingHTTPServer):
    """客户端断开/服务端主动断流都会抛 ConnectionError，那属于预期行为，别刷 traceback。"""

    daemon_threads = True

    def handle_error(self, request, client_address):
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionError, TimeoutError, OSError)):
            return
        super().handle_error(request, client_address)


def start_server(files, port=0, **cfg):
    cfg["files"] = files
    httpd = QuietServer(("127.0.0.1", port), Handler)
    httpd.state = State(cfg)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd


# ---------------------------------------------------------------- 用例执行


def env_with_path():
    env = dict(os.environ)
    env["PATH"] = MINGW + os.pathsep + env.get("PATH", "")
    return env


def run_minedown(base_url, root, log_path, timeout=600):
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    cmd = [EXE, "--selftest",
           "--mirror", "custom",
           "--custom-base", base_url,
           "--root", root,
           "--version", VERSION,
           "--log", log_path]
    t0 = time.time()
    p = subprocess.run(cmd, env=env_with_path(), timeout=timeout,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    elapsed = time.time() - t0
    try:
        with open(log_path, "r", encoding="utf-8") as f:
            log = f.read()
    except OSError:
        log = ""
    return p.returncode, elapsed, log


def fresh_dir(name):
    d = os.path.join(HERE, "_run", name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d, exist_ok=True)
    return d


FAILURES = []


def check(cond, msg):
    if cond:
        print(f"    ok   {msg}")
    else:
        print(f"    FAIL {msg}")
        FAILURES.append(msg)


RESULT_RE = re.compile(r"RESULT OK：共 (\d+) 个文件：完成 (\d+)，跳过 (\d+)，失败 (\d+)")


def final_failures(text):
    """返回最终汇总里的失败文件数；找不到汇总行返回 None。"""
    m = RESULT_RE.search(text)
    return None if m is None else int(m.group(4))


def scenario(name, files, meta, **cfg):
    print(f"\n[{name}]")
    httpd = start_server(files, **cfg)
    port = httpd.server_address[1]
    base = f"http://127.0.0.1:{port}"
    root = os.path.join(fresh_dir(name), "mc")
    log = os.path.join(HERE, "_run", name, "selftest.log")
    try:
        code, elapsed, text = run_minedown(base, root, log)
        return code, elapsed, text, base, root, log
    finally:
        httpd.shutdown()
        httpd.server_close()


def main():
    if not os.path.exists(EXE):
        print(f"找不到 {EXE}，先运行 build.bat")
        return 2

    print("生成测试数据集…")
    files, meta = build_dataset()
    total = sum(len(v) for v in files.values())
    print(f"  服务器路径 {len(files)} 个，共 {total / 1024 / 1024:.1f} MB"
          f"（client.jar {CLIENT_BYTES // 1024 // 1024} MB，大资源 "
          f"{[b // 1024 // 1024 for b in BIG_ASSET_BYTES]} MB）")

    want = [a.lower() for a in sys.argv[1:]]

    def wanted(tag):
        return not want or any(w in tag.lower() for w in want)

    # ---- 1. 全量下载：验证分块并行
    if wanted("chunk"):
        code, elapsed, text, base, root, log = scenario("01-chunk", files, meta)
        check(code == 0, f"退出码 0（实际 {code}）")
        check("分 4 段并行下载" in text, "client.jar 走了 4 段并行")
        jar = os.path.join(root, "versions", VERSION, f"{VERSION}.jar")
        check(os.path.exists(jar) and os.path.getsize(jar) == CLIENT_BYTES,
              "client.jar 完整落盘")
        check(final_failures(text) == 0, "最终失败 0 个文件")
        print(f"    用时 {elapsed:.1f}s")

    # ---- 2. 二次运行：全部命中跳过
    if wanted("skip"):
        root = os.path.join(HERE, "_run", "01-chunk", "mc")
        if not os.path.isdir(root):
            print("    跳过：先跑 chunk 场景")
        else:
            print("\n[02-skip]")
            httpd = start_server(files)
            port = httpd.server_address[1]
            log = os.path.join(HERE, "_run", "02-skip", "selftest.log")
            try:
                code, elapsed, text = run_minedown(f"http://127.0.0.1:{port}", root, log)
            finally:
                httpd.shutdown()
                httpd.server_close()
            check(code == 0, f"退出码 0（实际 {code}）")
            check(final_failures(text) == 0, "最终失败 0 个文件")
            check(elapsed < 30, f"二次运行很快（{elapsed:.1f}s）")
            check("分 4 段" not in text, "没有重新分块下载")

    # ---- 3. 断点续传：手工造 .part
    if wanted("resume"):
        big = meta["big"][0]
        dest = os.path.join(HERE, "_run", "01-chunk", "mc", "assets", "objects",
                            big["hash"][:2], big["hash"])
        if not os.path.exists(dest):
            print("\n[03-resume] 跳过：先跑 chunk 场景")
        else:
            print("\n[03-resume]")
            half = big["size"] // 2
            with open(dest, "rb") as f:
                head = f.read(half)
            os.remove(dest)
            with open(dest + ".part", "wb") as f:
                f.write(head)
            httpd = start_server(files, rate=6 * 1024 * 1024)
            port = httpd.server_address[1]
            log = os.path.join(HERE, "_run", "03-resume", "selftest.log")
            try:
                code, elapsed, text = run_minedown(
                    f"http://127.0.0.1:{port}",
                    os.path.join(HERE, "_run", "01-chunk", "mc"), log)
            finally:
                httpd.shutdown()
                httpd.server_close()
            check(code == 0, f"退出码 0（实际 {code}）")
            check(os.path.exists(dest) and os.path.getsize(dest) == big["size"],
                  "续传后文件完整")
            with open(dest, "rb") as f:
                check(sha1(f.read()) == big["hash"], "续传后 SHA1 正确")
            check(not os.path.exists(dest + ".part"), ".part 已清理")

    # ---- 4. 429 限流：应退避重试后成功
    if wanted("ratelimit"):
        asset_paths = {f"/{a['hash'][:2]}/{a['hash']}" for a in meta["assets"]}
        code, elapsed, text, base, root, log = scenario(
            "04-ratelimit", files, meta, fail_first=24, asset_paths=asset_paths)
        check(code == 0, f"退出码 0（实际 {code}）")
        check("429" in text or "后重试" in text, "日志里出现 429/重试")
        check(final_failures(text) == 0, "限流后仍全部成功")

    # ---- 5. 坏数据：SHA1 校验必须拦下
    if wanted("corrupt"):
        victim = meta["assets"][0]
        code, elapsed, text, base, root, log = scenario(
            "05-corrupt", files, meta, corrupt=f"/{victim['hash'][:2]}/{victim['hash']}")
        check(code != 0, f"退出码非 0（实际 {code}）")
        check("校验失败" in text, "报出校验失败")
        dest = os.path.join(root, "assets", "objects", victim["hash"][:2], victim["hash"])
        check(not os.path.exists(dest), "坏文件没有落盘")

    # ---- 6. 传输中断：断了要能重连续上
    if wanted("drop"):
        code, elapsed, text, base, root, log = scenario(
            "06-drop", files, meta, drop_after=2 * 1024 * 1024,
            drop_match=meta["big"][0]["hash"])
        check(code == 0, f"退出码 0（实际 {code}）")
        big = meta["big"][0]
        dest = os.path.join(root, "assets", "objects", big["hash"][:2], big["hash"])
        check(os.path.exists(dest) and os.path.getsize(dest) == big["size"],
              "中断后仍下完完整文件")

    # ---- 7. 权限：目录拒绝写入时立刻失败、已校验的 .part 必须留着
    if wanted("perm"):
        print("\n[07-perm]")
        big = meta["big"][0]
        httpd = start_server(files)
        port = httpd.server_address[1]
        base = f"http://127.0.0.1:{port}"
        root = os.path.join(fresh_dir("07-perm"), "mc")
        log = os.path.join(HERE, "_run", "07-perm", "selftest.log")
        dest = os.path.join(root, "assets", "objects", big["hash"][:2], big["hash"])
        user = os.environ.get("USERNAME", "")
        denied = False
        try:
            code, elapsed, text = run_minedown(base, root, log)
            check(code == 0, "先完整下载一次")
            # 目标文件挪走、留一份完整的 .part，再禁止往这个目录新建文件：
            # 改名必然被拒，客户端应该立刻如实上报，而不是空转 6 秒再重试 4 次。
            os.replace(dest, dest + ".part")
            r = subprocess.run(["icacls", os.path.dirname(dest), "/deny", user + ":(WD,AD)"],
                               capture_output=True, text=True)
            denied = r.returncode == 0
            if not denied:
                print("    跳过：当前账户无法设置拒绝 ACE（需要文件属主权限）")
            else:
                code, elapsed, text = run_minedown(base, root, log)
                check(code != 0, f"权限失败时退出码非 0（实际 {code}）")
                check("无法替换" in text or "无法删除" in text, "如实报出权限问题")
                check("拒绝访问" in text, "日志里点明「拒绝访问」")
                check(elapsed < 15, f"没有空转重试（{elapsed:.1f}s）")
                check(os.path.exists(dest + ".part"), "已校验的 .part 没有被删掉")
        finally:
            if denied:
                subprocess.run(["icacls", os.path.dirname(dest), "/remove:d", user],
                               capture_output=True, text=True)
            httpd.shutdown()
            httpd.server_close()

    print("\n" + "=" * 60)
    if FAILURES:
        print(f"失败 {len(FAILURES)} 项：")
        for f in FAILURES:
            print("  - " + f)
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    # `--serve [端口] [每连接限速字节/秒]`：手动起一个 mock 源，
    # 方便配合 GUI（--custom-base）做人工/截图验证。
    if len(sys.argv) > 1 and sys.argv[1] == "--serve":
        port = int(sys.argv[2]) if len(sys.argv) > 2 else 18080
        rate = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        files, meta = build_dataset()
        httpd = start_server(files, port=port, rate=rate)
        print(f"mock 源已启动：http://127.0.0.1:{httpd.server_address[1]}  "
              f"版本 {VERSION}  限速 {rate or '无'} 字节/秒/连接", flush=True)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            httpd.shutdown()
        sys.exit(0)
    sys.exit(main())
