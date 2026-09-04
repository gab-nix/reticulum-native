#!/usr/bin/env python3
"""C browser acceptance against pinned, unmodified NomadNet page serving code."""
import argparse
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import types

from test_lxmf_direct_live import check_checkout, reserve_port, RNS_COMMIT

NOMADNET_COMMIT = "475c0ee2a0388cf8470e7f1e90d5decb67b579ea"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reticulum", type=pathlib.Path, required=True)
    parser.add_argument("--nomadnet", type=pathlib.Path, required=True)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--transport", choices=("udp", "tcp"), required=True)
    args = parser.parse_args()
    check_checkout(args.reticulum, RNS_COMMIT)
    check_checkout(args.nomadnet, NOMADNET_COMMIT)
    sys.path.insert(0, str(args.reticulum.resolve()))
    import RNS
    if not pathlib.Path(RNS.__file__).resolve().is_relative_to(args.reticulum.resolve()):
        raise RuntimeError("RNS imported outside pinned checkout")
    spec = importlib.util.spec_from_file_location("pinned_node", args.nomadnet / "nomadnet/Node.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix="nomad-page-interop-") as temp:
        root = pathlib.Path(temp)
        c_port = reserve_port("udp") if args.transport == "udp" else 0
        py_port = reserve_port(args.transport)
        while py_port == c_port:
            py_port = reserve_port(args.transport)
        config = (f"type = TCPServerInterface\nlisten_ip = 127.0.0.1\nlisten_port = {py_port}\n"
                  if args.transport == "tcp" else
                  f"type = UDPInterface\nlisten_ip = 127.0.0.1\nlisten_port = {py_port}\n"
                  f"forward_ip = 127.0.0.1\nforward_port = {c_port}\n")
        (root / "config").write_text("[reticulum]\nshare_instance = No\nenable_transport = No\n"
            "[logging]\nloglevel = 0\n[interfaces]\n[[page-test]]\ninterface_enabled = True\n" + config)
        reticulum = RNS.Reticulum(configdir=str(root), loglevel=0)
        pages = root / "pages"
        pages.mkdir()
        (pages / "index.mu").write_bytes(b"Small page")
        seed = 0x13579BDF
        large = bytearray()
        for line in range(100):
            for _ in range(64):
                seed ^= (seed << 13) & 0xffffffff
                seed ^= seed >> 17
                seed ^= (seed << 5) & 0xffffffff
                large.append(ord("A") + seed % 26)
            if line != 99:
                large.append(10)
        (pages / "large.mu").write_bytes(large)
        # Exercise the unmodified registered handlers without starting the
        # application scheduler or enabling unrelated hosted services.
        node = module.Node.__new__(module.Node)
        node.app = types.SimpleNamespace(pagespath=str(pages),
            peer_settings={"served_page_requests": 0}, save_peer_settings=lambda: None)
        identity = RNS.Identity()
        node.destination = RNS.Destination(identity, RNS.Destination.IN,
            RNS.Destination.SINGLE, "nomadnetwork", "node")
        links = []
        node.destination.set_link_established_callback(links.append)
        node.register_pages()
        process = subprocess.Popen([str(args.driver.resolve()), args.transport,
            str(c_port), str(py_port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            started = time.monotonic()
            while process.poll() is None and time.monotonic() - started < 70:
                node.destination.announce(app_data=b"Synthetic page node")
                time.sleep(1)
            if process.poll() is None:
                process.kill()
            stdout, stderr = process.communicate(timeout=5)
            report = json.loads(stdout)
            report.update(transport=args.transport, rns_commit=RNS_COMMIT,
                          nomadnet_commit=NOMADNET_COMMIT,
                          links=len(links),
                          served=node.app.peer_settings["served_page_requests"])
            report["ok"] = bool(report.get("ok") and process.returncode == 0 and
                                not stderr and report["served"] >= 2 and len(links) == 1)
            print(json.dumps(report, sort_keys=True))
            return 0 if report["ok"] else 1
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            reticulum.exit_handler()


if __name__ == "__main__":
    raise SystemExit(main())
