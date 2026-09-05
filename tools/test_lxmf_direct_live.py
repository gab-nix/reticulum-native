#!/usr/bin/env python3
"""Opt-in real C↔pinned Python direct LXMF messaging acceptance test.

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
SEGMENTED_FIELD_SIZE = (1024 * 1024 - 1) + 4096


def check_checkout(path, commit):
    actual = subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if actual != commit:
        raise RuntimeError(f"Pinned revision mismatch: {path}")
    if subprocess.check_output(
            ["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"],
            text=True).strip():
        raise RuntimeError(f"Pinned source has tracked modifications: {path}")


def reserve_port(transport):
    socket_type = socket.SOCK_STREAM if transport == "tcp" else socket.SOCK_DGRAM
    with socket.socket(socket.AF_INET, socket_type) as sock:
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


def make_segmented_field():
    seed = 0x6D2B79F5
    result = bytearray()
    for _ in range(SEGMENTED_FIELD_SIZE):
        seed ^= (seed << 13) & 0xFFFFFFFF
        seed ^= seed >> 17
        seed ^= (seed << 5) & 0xFFFFFFFF
        result.append(seed & 0xFF)
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reticulum", type=pathlib.Path, required=True)
    parser.add_argument("--lxmf", type=pathlib.Path, required=True)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--transport", choices=("udp", "tcp"), default="udp")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--delay-inbound-ms", type=int, default=0,
                        help="Synthetic Python delivery-worker delay (0..1000 ms)")
    args = parser.parse_args()
    if not 0 <= args.delay_inbound_ms <= 1000:
        parser.error("--delay-inbound-ms must be between 0 and 1000")
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
              "transport": f"loopback {args.transport.upper()}",
              "c_events": [], "python_received": [],
              "python_proved": [], "errors": [], "ok": False}
    report["run_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report["synthetic_inbound_delay_ms"] = args.delay_inbound_ms
    report["library_revision"] = subprocess.check_output(
        ["git", "-C", str(pathlib.Path(__file__).resolve().parents[1]),
         "rev-parse", "HEAD"], text=True).strip()
    report["driver_source_sha256"] = hashlib.sha256(
        (pathlib.Path(__file__).resolve().parents[1] / "tests" / "interop" /
         "lxmf_direct_live_driver.c").read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="lxmf-direct-live-") as temp:
        root = pathlib.Path(temp)
        if args.transport == "tcp":
            c_port, py_port = None, reserve_port("tcp")
        else:
            c_port, py_port = reserve_port("udp"), reserve_port("udp")
            while c_port == py_port:
                py_port = reserve_port("udp")
        (root / "rns").mkdir()
        if args.transport == "tcp":
            interface_config = (
                "type = TCPServerInterface\ninterface_enabled = True\n"
                f"listen_ip = 127.0.0.1\nlisten_port = {py_port}\n")
        else:
            interface_config = (
                "type = UDPInterface\ninterface_enabled = True\n"
                f"listen_ip = 127.0.0.1\nlisten_port = {py_port}\n"
                f"forward_ip = 127.0.0.1\nforward_port = {c_port}\n")
        (root / "rns" / "config").write_text(
            "[reticulum]\nshare_instance = No\nenable_transport = No\n"
            "[logging]\nloglevel = 0\n[interfaces]\n[[C direct test]]\n" +
            interface_config)
        reticulum = RNS.Reticulum(configdir=str(root / "rns"), loglevel=0)
        identity = RNS.Identity()
        router = LXMF.LXMRouter(identity=identity, storagepath=str(root),
                                 autopeer=False, enforce_stamps=True,
                                 delivery_limit=2000)
        if args.delay_inbound_ms:
            original_delivery = router.lxmf_delivery

            def delayed_delivery(*delivery_args, **delivery_kwargs):
                time.sleep(args.delay_inbound_ms / 1000.0)
                return original_delivery(*delivery_args, **delivery_kwargs)

            router.lxmf_delivery = delayed_delivery
        source = router.register_delivery_identity(identity, "Python live test", stamp_cost=None)
        segmented_field = make_segmented_field()

        def received(message):
            index = len(report["python_received"])
            expected_size = 17 if index == 0 else 2048 if index == 1 else 23
            expected = make_body(expected_size, False)
            expected_fields = ({0x1234: bytes([1, 2, 3])} if index < 2
                               else {0x1234: segmented_field})
            ordinary_fields = dict(message.fields)
            ticket = ordinary_fields.pop(LXMF.FIELD_TICKET, None)
            ticket_valid = (isinstance(ticket, list) and len(ticket) == 2
                            and ticket[0] > time.time() and isinstance(ticket[1], bytes)
                            and len(ticket[1]) == LXMF.LXMessage.TICKET_LENGTH) if index == 0 else ticket is None
            stamp_valid = index == 0 or (message.stamp_valid and
                message.stamp is not None and len(message.stamp) == LXMF.LXMessage.TICKET_LENGTH)
            valid = (message.signature_validated and message.content == expected
                     and message.title == b"c-live" and ordinary_fields == expected_fields
                     and ticket_valid and stamp_valid
                     and message.method == LXMF.LXMessage.DIRECT)
            report["python_received"].append({"size": len(message.content),
                "verified": bool(message.signature_validated), "id": message.hash.hex(),
                "representation": message.representation, "valid": bool(valid),
                "ticket_present": ticket is not None, "ticket_stamp_valid": bool(index > 0 and stamp_valid)})
            # Bootstrap with one unstamped ticket offer, then require stamps.
            source.stamp_cost = 1
            if not valid:
                report["errors"].append("Python rejected content/signature/method expectations")

        router.register_delivery_callback(received)
        driver_command = ([str(args.driver.resolve()), "--tcp", str(py_port),
                           str(root / "c-messages.store")]
                          if args.transport == "tcp" else
                          [str(args.driver.resolve()), str(c_port), str(py_port),
                           str(root / "c-messages.store")])
        process = subprocess.Popen(driver_command,
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
            while time.monotonic() - started < 300:
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
                remote = RNS.Identity.recall(destination_hash) if destination_hash else None
                c_delivered = sum(1 for event in report["c_events"]
                                  if event.get("event") == "state" and
                                  event.get("state") == 3)
                # C starts sending as soon as it learns this announce. Only
                # release it after Python has validated C's identity, or a fast
                # stream connection can deliver the first LXMF before recall.
                if remote and now - last_announce > 3:
                    router.announce(source.hash)
                    last_announce = now
                if (remote and peer_verified and c_delivered > len(outbound) and
                        len(report["python_received"]) > len(outbound) and
                        len(outbound) < 3 and
                        len(report["python_proved"]) == len(outbound)):
                    # Python proves a packet before its delivery worker validates
                    # and remembers the ticket. Wait for that worker's callback,
                    # not merely C's proof event, before packing a ticket reply.
                    if router.get_outbound_ticket(destination_hash) is None:
                        report["errors"].append("Validated inbound message did not provide an outbound ticket")
                        break
                    destination = RNS.Destination(remote, RNS.Destination.OUT,
                        RNS.Destination.SINGLE, "lxmf", "delivery")
                    size = 17 if len(outbound) == 0 else 2048 if len(outbound) == 1 else 23
                    content = make_body(size, True)
                    outbound_fields = ({0x1234: bytes([1, 2, 3])}
                                       if len(outbound) < 2
                                       else {0x1234: segmented_field})
                    message = LXMF.LXMessage(destination, source, content,
                        title="python-live", fields=outbound_fields,
                        desired_method=LXMF.LXMessage.DIRECT,
                        include_ticket=len(outbound) == 0, stamp_cost=1)

                    def proved(sent):
                        report["python_proved"].append({"id": sent.hash.hex(),
                            "state": sent.state, "representation": sent.representation,
                            "size": len(sent.content),
                            "resource_parts": (sent.resource_representation.total_parts
                                if sent.resource_representation else 0),
                            "resource_segments": (sent.resource_representation.total_segments
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
                and len(report["python_received"]) == 3 and len(report["python_proved"]) == 3
                and all(e["valid"] for e in report["python_received"])
                and [e["representation"] for e in report["python_proved"]]
                    == [LXMF.LXMessage.PACKET, LXMF.LXMessage.RESOURCE,
                        LXMF.LXMessage.RESOURCE]
                and all(e["state"] == LXMF.LXMessage.DELIVERED for e in report["python_proved"])
                and report["python_proved"][-1]["resource_parts"] > 1
                and report["python_proved"][-1]["resource_segments"] > 1
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
