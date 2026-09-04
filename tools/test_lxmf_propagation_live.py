#!/usr/bin/env python3
"""Opt-in C to pinned-Python LXMF propagation-node acceptance test."""
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
    if (path / ".git").exists():
        actual = subprocess.check_output(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            text=True).strip()
        if actual != commit:
            raise RuntimeError(f"Pinned revision mismatch: {path}")
        dirty = subprocess.check_output(
            ["git", "-C", str(path), "status", "--porcelain",
             "--untracked-files=no"], text=True).strip()
        if dirty:
            raise RuntimeError(f"Pinned source has tracked modifications: {path}")
        return "git checkout"

    marker = path.parent / f"{path.name}.provenance"
    if not marker.is_file():
        raise RuntimeError(f"Missing pinned-source provenance: {path}")
    fields = dict(line.split("=", 1) for line in marker.read_text().splitlines()
                  if "=" in line)
    if fields.get("commit") != commit or fields.get("tracked_status") != "clean":
        raise RuntimeError(f"Invalid pinned-source provenance: {marker}")
    return "recorded clean source tree"


def reserve_port(transport):
    socket_type = socket.SOCK_STREAM if transport == "tcp" else socket.SOCK_DGRAM
    with socket.socket(socket.AF_INET, socket_type) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def make_body(length, from_python):
    seed = 0xA17E2C39 if from_python else 0x4D3B2A19
    result = bytearray()
    for _ in range(length):
        seed ^= (seed << 13) & 0xFFFFFFFF
        seed ^= seed >> 17
        seed ^= (seed << 5) & 0xFFFFFFFF
        result.append(33 + seed % 90)
    return bytes(result)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reticulum", type=pathlib.Path, required=True)
    parser.add_argument("--lxmf", type=pathlib.Path, required=True)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--transport", choices=("udp", "tcp"), default="udp")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    rns_source_evidence = check_checkout(args.reticulum, RNS_COMMIT)
    lxmf_source_evidence = check_checkout(args.lxmf, LXMF_COMMIT)
    sys.path[:0] = [str(args.reticulum.resolve()), str(args.lxmf.resolve())]
    import RNS
    import LXMF
    import LXMF.LXStamper as LXStamper
    for module, root in ((RNS, args.reticulum), (LXMF, args.lxmf)):
        if not pathlib.Path(module.__file__).resolve().is_relative_to(
                root.resolve()):
            raise RuntimeError("Imported implementation outside pinned checkout")

    repository = pathlib.Path(__file__).resolve().parents[1]
    driver_source = repository / "tests" / "interop" / \
        "lxmf_propagation_live_driver.c"
    report = {
        "rns_commit": RNS_COMMIT,
        "lxmf_commit": LXMF_COMMIT,
        "rns_version": RNS.__version__,
        "lxmf_version": LXMF.__version__,
        "rns_source_evidence": rns_source_evidence,
        "lxmf_source_evidence": lxmf_source_evidence,
        "transport": f"loopback {args.transport.upper()}",
        "run_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "native_head": subprocess.check_output(
            ["git", "-C", str(repository), "rev-parse", "HEAD"],
            text=True).strip(),
        "driver_source_sha256": sha256(driver_source),
        "harness_source_sha256": sha256(pathlib.Path(__file__)),
        "c_events": [],
        "python_received": [],
        "errors": [],
        "ok": False,
    }

    with tempfile.TemporaryDirectory(prefix="lxmf-propagation-live-") as temp:
        temp_root = pathlib.Path(temp)
        if args.transport == "tcp":
            c_port, python_port = None, reserve_port("tcp")
        else:
            c_port, python_port = reserve_port("udp"), reserve_port("udp")
            while c_port == python_port:
                python_port = reserve_port("udp")
        rns_dir = temp_root / "rns"
        rns_dir.mkdir()
        if args.transport == "tcp":
            interface_config = (
                "type = TCPServerInterface\ninterface_enabled = True\n"
                f"listen_ip = 127.0.0.1\nlisten_port = {python_port}\n")
        else:
            interface_config = (
                "type = UDPInterface\ninterface_enabled = True\n"
                f"listen_ip = 127.0.0.1\nlisten_port = {python_port}\n"
                f"forward_ip = 127.0.0.1\nforward_port = {c_port}\n")
        (rns_dir / "config").write_text(
            "[reticulum]\nshare_instance = No\nenable_transport = No\n"
            "[logging]\nloglevel = 0\n[interfaces]\n"
            "[[C propagation test]]\n" + interface_config)
        reticulum = RNS.Reticulum(configdir=str(rns_dir), loglevel=0)
        identity = RNS.Identity()
        router = LXMF.LXMRouter(
            identity=identity, storagepath=str(temp_root), autopeer=False,
            enforce_stamps=False,
            propagation_cost=LXMF.LXMRouter.PROPAGATION_COST_MIN)
        source = router.register_delivery_identity(
            identity, "Python propagation test", stamp_cost=None)
        router.enable_propagation()

        def received(message):
            expected = make_body(257, False)
            valid = (message.signature_validated and
                     message.method == LXMF.LXMessage.PROPAGATED and
                     message.content == expected and
                     message.title == b"c-pn" and
                     message.fields == {0x1234: bytes((1, 2, 3))})
            report["python_received"].append({
                "id": message.hash.hex(), "size": len(message.content),
                "method": message.method,
                "signature_valid": bool(message.signature_validated),
                "valid": bool(valid)})
            if not valid:
                report["errors"].append(
                    "Python rejected C upload content/signature/method")

        router.register_delivery_callback(received)
        driver_command = (
            [str(args.driver.resolve()), "--tcp", str(python_port),
             str(temp_root / "c.store")]
            if args.transport == "tcp" else
            [str(args.driver.resolve()), str(c_port), str(python_port),
             str(temp_root / "c.store")])
        process = subprocess.Popen(
            driver_command,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        lines = queue.Queue()

        def read_output():
            for line in process.stdout:
                lines.put(line)

        threading.Thread(target=read_output, daemon=True).start()
        c_destination = None
        injected = False
        injected_id = None
        last_announce = 0.0
        started = time.monotonic()
        try:
            while time.monotonic() - started < 200.0:
                while not lines.empty():
                    line = lines.get_nowait()
                    try:
                        event = json.loads(line)
                    except json.JSONDecodeError:
                        report["errors"].append(
                            "Unexpected non-JSON C diagnostic")
                        continue
                    report["c_events"].append(event)
                    if event.get("event") == "ready":
                        c_destination = bytes.fromhex(event["destination"])

                remote = (RNS.Identity.recall(c_destination)
                          if c_destination is not None else None)
                if remote is not None and not injected:
                    destination = RNS.Destination(
                        remote, RNS.Destination.OUT, RNS.Destination.SINGLE,
                        "lxmf", "delivery")
                    message = LXMF.LXMessage(
                        destination, source, make_body(2048, True),
                        title="python-pn", fields={0x1234: bytes((1, 2, 3))},
                        desired_method=LXMF.LXMessage.PROPAGATED)
                    message.pack()
                    stamp = message.get_propagation_stamp(
                        router.propagation_stamp_cost)
                    encrypted = (message.packed[:LXMF.LXMessage.DESTINATION_LENGTH]
                        + message._LXMessage__pn_encrypted_data)
                    workblock = LXStamper.stamp_workblock(
                        message.transient_id,
                        expand_rounds=LXStamper.WORKBLOCK_EXPAND_ROUNDS_PN)
                    if stamp is None or not LXStamper.stamp_valid(
                            stamp, router.propagation_stamp_cost, workblock):
                        report["errors"].append(
                            "Python failed to create valid injected PN stamp")
                    elif not router.lxmf_propagation(
                            encrypted,
                            stamp_value=message.propagation_stamp_value,
                            stamp_data=stamp):
                        report["errors"].append(
                            "Python propagation node rejected injected message")
                    else:
                        injected = True
                        injected_id = message.transient_id
                        report["python_injected"] = {
                            "transient_id": injected_id.hex(),
                            "message_id": message.hash.hex(),
                            "size": len(message.content),
                            "stamp_cost": router.propagation_stamp_cost}

                now = time.monotonic()
                if injected and now - last_announce >= 2.0:
                    router.announce(source.hash)
                    router.announce_propagation_node()
                    last_announce = now
                if process.poll() is not None:
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
            child_stderr = process.stderr.read().strip()
            if child_stderr:
                report["errors"].append("C driver emitted stderr diagnostics")
                report["c_stderr_sha256"] = hashlib.sha256(
                    child_stderr.encode()).hexdigest()
            done = [event for event in report["c_events"]
                    if event.get("event") == "done"]
            upload_states = [event for event in report["c_events"]
                             if event.get("event") == "upload_state"]
            sync_states = [event for event in report["c_events"]
                           if event.get("event") == "sync_done"]
            removed_after_ack = bool(
                injected_id is not None and
                injected_id not in router.propagation_entries)
            accepted_uploads = (router.client_propagation_messages_received +
                                router.unpeered_propagation_incoming)
            report["python_node"] = {
                "received_uploads": router.client_propagation_messages_received,
                "received_identified_unpeered_uploads":
                    router.unpeered_propagation_incoming,
                "accepted_uploads": accepted_uploads,
                "served_messages": router.client_propagation_messages_served,
                "ack_removed_injected": removed_after_ack}
            report["ok"] = bool(
                process.returncode == 0 and done and done[-1].get("ok") and
                len(report["python_received"]) == 1 and
                report["python_received"][0]["valid"] and upload_states and
                upload_states[-1].get("state") == 2 and sync_states and
                sync_states[-1].get("valid") and removed_after_ack and
                accepted_uploads >= 1 and
                router.client_propagation_messages_served >= 1 and
                not report["errors"])
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            stdout, stderr = sys.stdout, sys.stderr
            reticulum.exit_handler()
            sys.stdout, sys.stderr = stdout, stderr

    report["driver_sha256"] = sha256(args.driver)
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output is not None:
        args.output.write_text(rendered)
    print(rendered, end="", flush=True)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
