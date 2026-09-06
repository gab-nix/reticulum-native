#!/usr/bin/env python3
"""Pinned stock Python versus embedded packet endpoint, synthetic loopback UDP."""
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
from test_lxmf_direct_live import check_checkout, reserve_port, RNS_COMMIT, LXMF_COMMIT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reticulum", type=pathlib.Path, required=True)
    parser.add_argument("--lxmf", type=pathlib.Path, required=True)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    args = parser.parse_args()
    check_checkout(args.reticulum, RNS_COMMIT)
    check_checkout(args.lxmf, LXMF_COMMIT)
    sys.path[:0] = [str(args.reticulum.resolve()), str(args.lxmf.resolve())]
    import RNS
    import LXMF
    for module, root in ((RNS, args.reticulum), (LXMF, args.lxmf)):
        if not pathlib.Path(module.__file__).resolve().is_relative_to(root.resolve()):
            raise RuntimeError("Imported module is outside pinned checkout")
    report = {"rns_commit": RNS_COMMIT, "lxmf_commit": LXMF_COMMIT,
              "transport": "loopback UDP", "received": [], "proved": [],
              "c_events": [], "announces": 0, "ok": False}
    with tempfile.TemporaryDirectory(prefix="lxmf-packet-live-") as temp:
        root = pathlib.Path(temp)
        c_port, py_port = reserve_port("udp"), reserve_port("udp")
        while c_port == py_port:
            py_port = reserve_port("udp")
        (root / "rns").mkdir()
        (root / "rns" / "config").write_text(
            "[reticulum]\nshare_instance = No\nenable_transport = No\n"
            "[logging]\nloglevel = 0\n[interfaces]\n[[Embedded UDP]]\n"
            "type = UDPInterface\ninterface_enabled = True\n"
            f"listen_ip = 127.0.0.1\nlisten_port = {py_port}\n"
            f"forward_ip = 127.0.0.1\nforward_port = {c_port}\n")
        runtime = RNS.Reticulum(configdir=str(root / "rns"), loglevel=0)
        identity = RNS.Identity()
        router = LXMF.LXMRouter(identity=identity, storagepath=str(root),
                                autopeer=False, enforce_stamps=True)
        source = router.register_delivery_identity(identity, "Python packet test", stamp_cost=None)

        def received(message):
            index = len(report["received"])
            expected = b"hello" if index == 0 else b"again"
            report["received"].append({"valid": bool(message.signature_validated
                and message.content == expected and message.method == LXMF.LXMessage.DIRECT),
                "id": message.hash.hex(), "signature": bool(message.signature_validated),
                "content_matches": message.content == expected, "method": message.method,
                "representation": message.representation})

        router.register_delivery_callback(received)
        process = subprocess.Popen([str(args.driver.resolve()), str(c_port), str(py_port), source.hash.hex()],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        lines = queue.Queue()

        def read_output():
            for line in process.stdout:
                lines.put(line)

        threading.Thread(target=read_output, daemon=True).start()
        address, message = None, None
        started = time.monotonic()
        try:
            while time.monotonic() - started < 125:
                while not lines.empty():
                    event = json.loads(lines.get_nowait())
                    report["c_events"].append(event)
                    if event.get("event") == "ready":
                        address = bytes.fromhex(event["destination"])
                remote = RNS.Identity.recall(address) if address else None
                if remote and report["announces"] == 0:
                    router.announce(source.hash)
                    report["announces"] = 1
                if remote and any(event.get("event") == "peer_verified" for event in report["c_events"]) and message is None:
                    destination = RNS.Destination(remote, RNS.Destination.OUT,
                        RNS.Destination.SINGLE, "lxmf", "delivery")
                    message = LXMF.LXMessage(destination, source, "reply", desired_method=LXMF.LXMessage.DIRECT,
                                             include_ticket=False)
                    message.register_delivery_callback(lambda sent: report["proved"].append(
                        {"state": sent.state, "representation": sent.representation}))
                    router.handle_outbound(message)
                if process.poll() is not None:
                    time.sleep(0.05)
                    while not lines.empty():
                        report["c_events"].append(json.loads(lines.get_nowait()))
                    break
                time.sleep(0.01)
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
            report["exit"] = process.returncode
            report["stderr_present"] = bool(process.stderr.read().strip())
            report["ok"] = (process.returncode == 0 and len(report["received"]) == 2
                and all(event["valid"] for event in report["received"])
                and report["proved"] == [{"state": LXMF.LXMessage.DELIVERED,
                                            "representation": LXMF.LXMessage.PACKET}]
                and any(event.get("event") == "reboot" for event in report["c_events"])
                and any(event.get("event") == "inbound_link_request" for event in report["c_events"])
                and report["announces"] == 1 and not report["stderr_present"])
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            stdout, stderr = sys.stdout, sys.stderr
            runtime.exit_handler()
            sys.stdout, sys.stderr = stdout, stderr
    report["driver_sha256"] = hashlib.sha256(args.driver.read_bytes()).hexdigest()
    project = pathlib.Path(__file__).resolve().parents[1]
    report["driver_source_sha256"] = hashlib.sha256(
        (project / "tests/interop/lxmf_packet_live_driver.c").read_bytes()).hexdigest()
    report["library_revision"] = subprocess.check_output(
        ["git", "-C", str(project), "rev-parse", "HEAD"], text=True).strip()
    print(json.dumps(report, indent=2), flush=True)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
