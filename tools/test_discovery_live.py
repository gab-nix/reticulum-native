#!/usr/bin/env python3
"""Bidirectional path discovery with a pinned, unmodified Python Reticulum peer."""
import argparse
import hashlib
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading

from test_lxmf_direct_live import RNS_COMMIT, check_checkout, reserve_port
from test_hosted_live import wait_for


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
              "python_to_c": False, "c_to_python": False}
    project = pathlib.Path(__file__).resolve().parents[1]
    report["library_revision"] = subprocess.check_output(
        ["git", "-C", str(project), "rev-parse", "HEAD"], text=True).strip()
    report["driver_source_sha256"] = hashlib.sha256(
        (project / "tests" / "interop" / "discovery_live_driver.c").read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="synthetic-discovery-") as directory:
        root = pathlib.Path(directory)
        c_port, py_port = reserve_port(args.transport), reserve_port(args.transport)
        while c_port == py_port:
            py_port = reserve_port(args.transport)
        interface = (f"type = TCPServerInterface\nlisten_ip = 127.0.0.1\nlisten_port = {py_port}\n"
                     if args.transport == "tcp" else
                     f"type = UDPInterface\nlisten_ip = 127.0.0.1\nlisten_port = {py_port}\nforward_ip = 127.0.0.1\nforward_port = {c_port}\n")
        (root / "config").write_text(
            "[reticulum]\nshare_instance = No\nenable_transport = No\n[logging]\nloglevel = 0\n"
            "[interfaces]\n[[Synthetic discovery]]\ninterface_enabled = True\n" + interface)
        runtime = RNS.Reticulum(configdir=str(root), loglevel=0)
        identity = RNS.Identity()
        local = RNS.Destination(identity, RNS.Destination.IN, RNS.Destination.SINGLE, "lxmf", "delivery")
        process = subprocess.Popen([str(args.driver.resolve()),
            "--tcp" if args.transport == "tcp" else str(c_port), str(py_port)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        events = queue.Queue()

        def output():
            for line in process.stdout:
                try:
                    events.put(json.loads(line))
                except ValueError:
                    events.put({"event": "invalid"})

        threading.Thread(target=output, daemon=True).start()
        try:
            ready = events.get(timeout=10)
            assert ready["event"] == "ready"
            destination = bytes.fromhex(ready["destination"])
            wait_for(lambda: RNS.Identity.recall(destination) is not None and RNS.Transport.has_path(destination))
            # Simulate a peer missing the initial announcement, without patching
            # upstream behavior. Remove only this ephemeral synthetic peer's caches.
            with RNS.Identity.known_destinations_lock:
                RNS.Identity.known_destinations.pop(destination, None)
            with RNS.Transport.path_table_lock:
                RNS.Transport.path_table.pop(destination, None)
            assert RNS.Identity.recall(destination) is None and not RNS.Transport.has_path(destination)
            RNS.Transport.request_path(destination)
            wait_for(lambda: RNS.Identity.recall(destination) is not None and RNS.Transport.has_path(destination))
            assert RNS.Identity.recall(destination).get_public_key().hex() == ready["public"]
            report["python_to_c"] = True
            # This Python destination has never announced; its stock path
            # responder must supply the C node's verified identity and route.
            process.stdin.write("r" + local.hash.hex() + "\n")
            process.stdin.flush()
            learned = events.get(timeout=30)
            assert learned["event"] == "announce" and learned["destination"] == local.hash.hex()
            assert learned["public"] == identity.get_public_key().hex()
            report["c_to_python"] = True
            process.stdin.write("q\n")
            process.stdin.flush()
            process.wait(timeout=10)
            assert process.returncode == 0 and not process.stderr.read().strip()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            stdout, stderr = sys.stdout, sys.stderr
            runtime.exit_handler()
            sys.stdout, sys.stderr = stdout, stderr
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
