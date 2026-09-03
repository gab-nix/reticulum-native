#!/usr/bin/env python3
"""Opt-in real UDP C↔pinned Python direct LXMF messaging acceptance test.

Creates only ephemeral synthetic identities and data. JSON output includes
public message IDs and states, never keys, contents, or captured wire packets.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time

RNS_COMMIT = "ea98db4f53dcf0defc0e71a16e60d28b1229c4e6"
LXMF_COMMIT = "795fdaa2b0777c13033787d933d1afc94a2377cb"


def check_checkout(path, commit):
    actual = subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if actual != commit:
        raise RuntimeError(f"Pinned revision mismatch: {path}")
    if subprocess.check_output(
            ["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"],
            text=True).strip():
        raise RuntimeError(f"Pinned source has tracked modifications: {path}")


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def make_body(length, from_python):
    seed = 0x2468ACE1 if from_python else 0x13579BDF
    result = bytearray()
    for i in range(length):
        seed ^= (seed << 13) & 0xFFFFFFFF
        seed ^= seed >> 17
        seed ^= (seed << 5) & 0xFFFFFFFF
        result.append((ord("a" if from_python else "A") + i % 26)
                      if length < 100 else (33 + seed % 90))
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reticulum", type=pathlib.Path, required=True)
    parser.add_argument("--lxmf", type=pathlib.Path, required=True)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    check_checkout(args.reticulum, RNS_COMMIT)
    check_checkout(args.lxmf, LXMF_COMMIT)
    sys.path[:0] = [str(args.reticulum.resolve()), str(args.lxmf.resolve())]
    import RNS
    import LXMF
    for module, root in ((RNS, args.reticulum), (LXMF, args.lxmf)):
        if not pathlib.Path(module.__file__).resolve().is_relative_to(root.resolve()):
            raise RuntimeError("Imported implementation outside pinned checkout")
    report = {"rns_commit": RNS_COMMIT, "lxmf_commit": LXMF_COMMIT,
              "rns_version": RNS.__version__, "lxmf_version": LXMF.__version__,
              "transport": "loopback UDP", "c_events": [], "python_received": [],
              "python_proved": [], "errors": [], "ok": False}
    report["run_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report["library_revision"] = subprocess.check_output(
        ["git", "-C", str(pathlib.Path(__file__).resolve().parents[1]),
         "rev-parse", "HEAD"], text=True).strip()
    report["driver_source_sha256"] = hashlib.sha256(
        (pathlib.Path(__file__).resolve().parents[1] / "tests" / "interop" /
         "lxmf_direct_live_driver.c").read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="lxmf-direct-live-") as temp:
        root = pathlib.Path(temp)
        c_port, py_port = reserve_port(), reserve_port()
        while c_port == py_port:
            py_port = reserve_port()
        (root / "rns").mkdir()
        (root / "rns" / "config").write_text(
            "[reticulum]\nshare_instance = No\nenable_transport = No\n"
            "[logging]\nloglevel = 0\n[interfaces]\n[[C direct test]]\n"
            "type = UDPInterface\ninterface_enabled = True\n"
            f"listen_ip = 127.0.0.1\nlisten_port = {py_port}\n"
            f"forward_ip = 127.0.0.1\nforward_port = {c_port}\n")
        reticulum = RNS.Reticulum(configdir=str(root / "rns"), loglevel=0)
        identity = RNS.Identity()
        router = LXMF.LXMRouter(identity=identity, storagepath=str(root),
                                 autopeer=False, enforce_stamps=False)
        source = router.register_delivery_identity(identity, "Python live test", stamp_cost=None)

        def received(message):
            expected_size = 17 if not report["python_received"] else 2048
            expected = make_body(expected_size, False)
            valid = (message.signature_validated and message.content == expected
                     and message.title == b"c-live" and message.fields == {0x1234: bytes([1, 2, 3])}
                     and message.method == LXMF.LXMessage.DIRECT)
            report["python_received"].append({"size": len(message.content),
                "verified": bool(message.signature_validated), "id": message.hash.hex(),
                "representation": message.representation, "valid": bool(valid)})
            if not valid:
                report["errors"].append("Python rejected content/signature/method expectations")

        router.register_delivery_callback(received)
        process = subprocess.Popen([str(args.driver.resolve()), str(c_port), str(py_port),
                                    str(root / "c-messages.store")],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        lines = queue.Queue()

        def read_output():
            for line in process.stdout:
                lines.put(line)

        threading.Thread(target=read_output, daemon=True).start()
        destination_hash = None
        peer_verified = False
        outbound = []
        started = time.monotonic()
        last_announce = 0
        try:
            while time.monotonic() - started < 100:
                while not lines.empty():
                    line = lines.get_nowait()
                    try:
                        event = json.loads(line)
                    except json.JSONDecodeError:
                        report["errors"].append("Unexpected non-JSON C diagnostic")
                        continue
                    report["c_events"].append(event)
                    if event.get("event") == "ready":
                        destination_hash = bytes.fromhex(event["destination"])
                    if event.get("event") == "peer_verified":
                        peer_verified = True
                now = time.monotonic()
                if now - last_announce > 3:
                    router.announce(source.hash)
                    last_announce = now
                remote = RNS.Identity.recall(destination_hash) if destination_hash else None
                if remote and peer_verified and len(outbound) < 2 and len(report["python_proved"]) == len(outbound):
                    destination = RNS.Destination(remote, RNS.Destination.OUT,
                        RNS.Destination.SINGLE, "lxmf", "delivery")
                    size = 17 if not outbound else 2048
                    content = make_body(size, True)
                    message = LXMF.LXMessage(destination, source, content,
                        title="python-live", fields={0x1234: bytes([1, 2, 3])},
                        desired_method=LXMF.LXMessage.DIRECT)

                    def proved(sent):
                        report["python_proved"].append({"id": sent.hash.hex(),
                            "state": sent.state, "representation": sent.representation,
                            "size": len(sent.content),
                            "resource_parts": (sent.resource_representation.total_parts
                                if sent.resource_representation else 0)})

                    def failed(sent):
                        report["errors"].append(f"Python outbound failed state {sent.state}")

                    message.register_delivery_callback(proved)
                    message.register_failed_callback(failed)
                    outbound.append(message)
                    router.handle_outbound(message)
                if process.poll() is not None:
                    # Reader may still hold the final line for one scheduling turn.
                    time.sleep(0.05)
                    while not lines.empty():
                        report["c_events"].append(json.loads(lines.get_nowait()))
                    break
                if report["errors"]:
                    break
                time.sleep(0.01)
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
            report["c_exit"] = process.returncode
            if process.stderr.read().strip():
                report["errors"].append("C driver emitted stderr diagnostics")
            done = [e for e in report["c_events"] if e.get("event") == "done"]
            report["ok"] = bool(process.returncode == 0 and done and done[-1]["ok"]
                and len(report["python_received"]) == 2 and len(report["python_proved"]) == 2
                and all(e["valid"] for e in report["python_received"])
                and [e["representation"] for e in report["python_proved"]]
                    == [LXMF.LXMessage.PACKET, LXMF.LXMessage.RESOURCE]
                and all(e["state"] == LXMF.LXMessage.DELIVERED for e in report["python_proved"])
                and report["python_proved"][-1]["resource_parts"] > 1
                and not report["errors"])
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            stdout, stderr = sys.stdout, sys.stderr
            reticulum.exit_handler()
            sys.stdout, sys.stderr = stdout, stderr
    report["driver_sha256"] = hashlib.sha256(args.driver.read_bytes()).hexdigest()
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="", flush=True)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
