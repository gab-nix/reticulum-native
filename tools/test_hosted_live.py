#!/usr/bin/env python3
"""Pinned Python Reticulum requests against an opt-in synthetic C page host."""
import argparse
import hashlib
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time

from test_lxmf_direct_live import RNS_COMMIT, check_checkout, reserve_port, make_body


def wait_for(predicate, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise RuntimeError("bounded hosted acceptance wait expired")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reticulum", type=pathlib.Path, required=True)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--transport", choices=("udp", "tcp"), required=True)
    args = parser.parse_args()
    check_checkout(args.reticulum, RNS_COMMIT)
    sys.path.insert(0, str(args.reticulum.resolve()))
    import RNS
    assert pathlib.Path(RNS.__file__).resolve().is_relative_to(args.reticulum.resolve())
    report = {"rns_commit": RNS_COMMIT, "transport": args.transport,
              "driver_sha256": hashlib.sha256(args.driver.read_bytes()).hexdigest(),
              "responses": [], "ok": False}
    project = pathlib.Path(__file__).resolve().parents[1]
    report["library_revision"] = subprocess.check_output(
        ["git", "-C", str(project), "rev-parse", "HEAD"], text=True).strip()
    report["driver_source_sha256"] = hashlib.sha256(
        (project / "tests" / "interop" / "hosted_live_driver.c").read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="synthetic-hosted-") as temporary:
        root = pathlib.Path(temporary)
        pages = root / "pages"
        pages.mkdir()
        small = b">Synthetic page\nNative hosted page\n"
        large = b">Large page\n" + make_body(65536, False)
        (pages / "index.mu").write_bytes(small)
        (pages / "large.mu").write_bytes(large)
        (pages / "form.mu").write_bytes(b"#!in-process-test-provider")
        (pages / "form.mu").chmod(0o700)
        (pages / "restricted.mu").write_bytes(b">Allowed visitor\n")
        c_port, py_port = reserve_port(args.transport), reserve_port(args.transport)
        while c_port == py_port:
            py_port = reserve_port(args.transport)
        (root / "rns").mkdir()
        interface = (f"type = TCPServerInterface\nlisten_ip = 127.0.0.1\nlisten_port = {py_port}\n"
                     if args.transport == "tcp" else
                     f"type = UDPInterface\nlisten_ip = 127.0.0.1\nlisten_port = {py_port}\nforward_ip = 127.0.0.1\nforward_port = {c_port}\n")
        (root / "rns" / "config").write_text(
            "[reticulum]\nshare_instance = No\nenable_transport = No\n[logging]\nloglevel = 0\n"
            "[interfaces]\n[[Synthetic host]]\ninterface_enabled = True\n" + interface)
        reticulum = RNS.Reticulum(configdir=str(root / "rns"), loglevel=0)
        visitor = RNS.Identity()
        (pages / "restricted.mu.allowed").write_text(visitor.hash.hex() + "\n")
        command = [str(args.driver.resolve()), "--tcp" if args.transport == "tcp" else str(c_port),
                   str(py_port), str(pages)]
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        events = queue.Queue()
        def read_output():
            for line in process.stdout:
                events.put(json.loads(line))
        threading.Thread(target=read_output, daemon=True).start()
        link = None
        try:
            ready = events.get(timeout=10)
            destination_hash = bytes.fromhex(ready["destination"])
            wait_for(lambda: RNS.Identity.recall(destination_hash) is not None and RNS.Transport.has_path(destination_hash))
            remote = RNS.Identity.recall(destination_hash)
            destination = RNS.Destination(remote, RNS.Destination.OUT, RNS.Destination.SINGLE,
                                          "nomadnetwork", "node")
            assert destination.hash == destination_hash
            # ACTIVE is set before Python sends LRRTT. The public callback
            # runs after it, so do not race a request ahead of confirmation.
            established = threading.Event()
            link = RNS.Link(destination, established_callback=lambda _: established.set())
            wait_for(established.is_set)

            def request(path, expected, data=None):
                receipt = link.request(path, data=data, timeout=20)
                assert receipt is not None
                wait_for(lambda: receipt.status in (RNS.RequestReceipt.READY, RNS.RequestReceipt.FAILED))
                assert receipt.status == RNS.RequestReceipt.READY
                assert receipt.response == expected
                if path == "/page/large.mu":
                    assert receipt.response_transfer_size > 500
                report["responses"].append({"path": path, "bytes": len(expected), "valid": True,
                                           "transfer_bytes": receipt.response_transfer_size})

            request("/page/index.mu", small)
            request("/page/large.mu", large)
            request("/page/form.mu", b"Rei:preview", {"field_name": "Rei", "var_action": "preview"})
            request("/page/form.mu", b"Rei:submit", {"field_name": "Rei", "var_action": "submit"})
            request("/page/restricted.mu", b">Request Not Allowed\n\nYou are not authorised to carry out the request.\n")
            link.identify(visitor)
            request("/page/restricted.mu", b">Allowed visitor\n")
            process.stdin.write("q")
            process.stdin.flush()
            process.wait(timeout=10)
            assert process.returncode == 0 and not process.stderr.read().strip()
            report["ok"] = True
        finally:
            if link is not None:
                link.teardown()
            if process.poll() is None:
                process.kill()
                process.wait()
            stdout, stderr = sys.stdout, sys.stderr
            reticulum.exit_handler()
            sys.stdout, sys.stderr = stdout, stderr
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
