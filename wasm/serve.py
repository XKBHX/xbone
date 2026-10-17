#!/usr/bin/env python3
"""Serve an Emscripten build so the page is cross-origin isolated.

Pthreads need SharedArrayBuffer. Browsers only enable that in a secure
context (https, or http://127.0.0.1 / localhost) when the document is
served with:

    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp

python3 -m http.server does not send those headers. emrun does; this
server sends the same isolation headers, including
Cross-Origin-Resource-Policy: cross-origin, and it sets the MIME types
a modularized build needs (.js as text/javascript, .wasm as
application/wasm).

Usage:
    wasm/serve.py [directory] [--port N] [--bind ADDR]
    wasm/serve.py --check

The default directory is build/wasm/dist. Copy wasm/index.html next to
qemu-system-i386.js before serving that directory.
"""

import argparse
import functools
import http.server
import os
import sys
import tempfile
import threading
import urllib.request


ISOLATION_HEADERS = (
    ("Cross-Origin-Opener-Policy", "same-origin"),
    ("Cross-Origin-Embedder-Policy", "require-corp"),
    ("Cross-Origin-Resource-Policy", "cross-origin"),
)

MIME_TYPES = {
    ".html": "text/html",
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".wasm": "application/wasm",
}


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        **MIME_TYPES,
    }

    def end_headers(self):
        for name, value in ISOLATION_HEADERS:
            self.send_header(name, value)
        self.send_header("Cache-Control", "no-cache, must-revalidate")
        super().end_headers()


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def make_server(directory, bind, port):
    handler = functools.partial(Handler, directory=directory)
    return http.server.ThreadingHTTPServer((bind, port), handler)


def check_response(url, mime):
    with urllib.request.urlopen(url) as resp:
        if resp.status != 200:
            raise SystemExit(f"{url} returned {resp.status}")
        for name, value in ISOLATION_HEADERS:
            got = resp.headers.get(name)
            if got != value:
                raise SystemExit(f"{url} {name}: {got!r}, expected {value!r}")
        got_mime = resp.headers.get_content_type()
        if got_mime != mime:
            raise SystemExit(f"{url} Content-Type: {got_mime!r}, expected {mime!r}")


def self_check():
    with tempfile.TemporaryDirectory() as directory:
        samples = {
            "index.html": b"<p>ok</p>",
            "app.js": b"export {}",
            "app.wasm": b"\0asm",
        }
        for name, data in samples.items():
            with open(os.path.join(directory, name), "wb") as f:
                f.write(data)
        server = make_server(directory, "127.0.0.1", 0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            port = server.server_address[1]
            base = f"http://127.0.0.1:{port}"
            check_response(f"{base}/index.html", "text/html")
            check_response(f"{base}/app.js", "text/javascript")
            check_response(f"{base}/app.wasm", "application/wasm")
        finally:
            server.shutdown()
            thread.join()
    print("wasm/serve.py: isolation headers and MIME types ok")


def main(argv):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", nargs="?",
                        default=os.path.join(repo_root(), "build", "wasm", "dist"))
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--check", action="store_true",
                        help="serve a temp directory and check the headers")
    args = parser.parse_args(argv)

    if args.check:
        self_check()
        return 0

    directory = os.path.abspath(args.directory)
    if not os.path.isdir(directory):
        print(f"not a directory: {directory}", file=sys.stderr)
        return 1

    server = make_server(directory, args.bind, args.port)
    host, port = server.server_address[:2]
    print(f"Serving {directory}", flush=True)
    print(f"http://{host}:{port}/", flush=True)
    print("Cross-Origin-Opener-Policy: same-origin", flush=True)
    print("Cross-Origin-Embedder-Policy: require-corp", flush=True)
    if args.bind not in ("127.0.0.1", "localhost", "::1"):
        print("A non-loopback http origin is not a secure context.",
              file=sys.stderr)
        print("SharedArrayBuffer stays disabled unless you use https.",
              file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
