#!/usr/bin/env python3
"""Opt-in pinned LXMF/RNS opportunistic UDP interoperability test.

All peers and identities are synthetic and temporary. The public report contains
only revision/binary fingerprints, message IDs, byte counts and check results.
No payload, key, packet capture, or upstream log is written to the report.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

RNS_COMMIT = "ea98db4f53dcf0defc0e71a16e60d28b1229c4e6"
LXMF_COMMIT = "795fdaa2b0777c13033787d933d1afc94a2377cb"


class VerificationFailure(Exception):
    """A static, privacy-safe failure code, never raw subprocess output."""


def require(condition, code):
    if not condition:
        raise VerificationFailure(code)


def check_sanitizers(stderr):
    require(not any(marker in stderr for marker in (
        b"AddressSanitizer", b"UndefinedBehaviorSanitizer", b"LeakSanitizer", b"runtime error:"
    )), "native_sanitizer_diagnostic")


def pinned_checkout(path, expected):
    result = subprocess.run(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        capture_output=True, text=True, timeout=5, check=False,
    )
    require(result.returncode == 0 and result.stdout.strip() == expected,
            "upstream_revision_mismatch")
    result = subprocess.run(
        ["git", "-C", str(path), "diff", "--quiet", "HEAD", "--"],
        capture_output=True, timeout=5, check=False,
    )
    require(result.returncode == 0, "upstream_tracked_changes")


def fingerprint(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as endpoint:
        endpoint.bind(("127.0.0.1", 0))
        return endpoint.getsockname()[1]


def stop_process_group(process):
    """Only receives a child created with start_new_session=True."""
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)


def worker(args):
    """Separate process keeps upstream singleton threads/atexit in isolation."""
    report = {"schema": 1, "result": "failed", "stage": "upstream_import"}
    report_path = args.work / "result.json"
    receiver = None
    reticulum = None
    try:
        sys.path[:0] = [str(args.reticulum), str(args.lxmf)]
        import RNS  # pylint: disable=import-outside-toplevel
        import LXMF  # pylint: disable=import-outside-toplevel

        require(Path(RNS.__file__).resolve().is_relative_to(args.reticulum),
                "wrong_rns_import")
        require(Path(LXMF.__file__).resolve().is_relative_to(args.lxmf),
                "wrong_lxmf_import")
        report["versions"] = {"reticulum": RNS.__version__, "lxmf": LXMF.__version__}
        deadline = time.monotonic() + args.timeout

        def command(arguments, code):
            remaining = deadline - time.monotonic()
            require(remaining > 0, "deadline_exceeded")
            result = subprocess.run(arguments, capture_output=True, timeout=remaining,
                                    check=False)
            check_sanitizers(result.stderr)
            require(result.returncode == 0, code)
            return result.stdout

        report["stage"] = "synthetic_identity"
        c_identity_path = args.work / "synthetic-c.identity"
        identity_output = command([str(args.rnid), "generate", str(c_identity_path)],
                                  "c_identity_generation_failed")
        match = re.search(rb"^Public\s+: ([0-9a-f]{128})$", identity_output, re.MULTILINE)
        require(match is not None, "c_identity_output_invalid")
        c_public = bytes.fromhex(match.group(1).decode("ascii"))
        c_address_hex = command([str(args.nomad_chat), "address", str(c_identity_path)],
                                "c_address_failed").strip().decode("ascii")
        require(re.fullmatch(r"[0-9a-f]{32}", c_address_hex) is not None,
                "c_address_output_invalid")

        report["stage"] = "python_runtime"
        python_port = reserve_port()
        c_port = reserve_port()
        require(python_port != c_port, "loopback_port_collision")
        config_dir = args.work / "rns"
        config_dir.mkdir(mode=0o700)
        (config_dir / "config").write_text(
            "[reticulum]\nshare_instance = No\nenable_transport = No\n"
            "[logging]\nloglevel = 0\n[interfaces]\n[[Synthetic UDP]]\n"
            "type = UDPInterface\nenabled = Yes\nlisten_ip = 127.0.0.1\n"
            f"listen_port = {python_port}\nforward_ip = 127.0.0.1\n"
            f"forward_port = {c_port}\n", encoding="utf-8",
        )
        reticulum = RNS.Reticulum(configdir=str(config_dir), loglevel=0)
        require(any(interface.online for interface in RNS.Transport.interfaces),
                "python_interface_offline")
        python_identity = RNS.Identity()
        router = LXMF.LXMRouter(identity=python_identity, storagepath=str(args.work),
                                enforce_ratchets=False, enforce_stamps=False)
        python_destination = router.register_delivery_identity(
            python_identity, display_name="Synthetic interoperability peer", stamp_cost=None)
        c_identity = RNS.Identity(create_keys=False)
        c_identity.load_public_key(c_public)
        c_destination = RNS.Destination(c_identity, RNS.Destination.OUT,
                                         RNS.Destination.SINGLE, "lxmf", "delivery")
        require(c_destination.hash.hex() == c_address_hex, "c_python_destination_mismatch")
        # Source identity is explicitly provisioned; this gate does not test
        # announce discovery, ratchet selection, paths or unknown senders.
        RNS.Identity.remember(bytes(32), c_destination.hash, c_public)
        nonce = os.urandom(8).hex()
        c_text = f"synthetic-c-to-python-{nonce}"
        python_text = f"synthetic-python-to-c-{nonce}"
        arrived = threading.Event()
        observed = []

        def delivered(message):
            observed.append(message)
            arrived.set()

        router.register_delivery_callback(delivered)
        report["stage"] = "c_to_python"
        command([str(args.nomad_chat), "send-udp", str(c_identity_path),
                 python_destination.hash.hex(), python_identity.get_public_key().hex(),
                 "127.0.0.1", str(python_port), c_text], "c_send_failed")
        require(arrived.wait(max(0, deadline - time.monotonic())), "python_receive_timeout")
        message = observed[0]
        require(message.signature_validated is True, "python_signature_rejected")
        require(message.source_hash == c_destination.hash, "python_wrong_source")
        require(message.destination_hash == python_destination.hash, "python_wrong_destination")
        require(message.content == c_text.encode(), "python_content_mismatch")
        report["c_to_python"] = {
            "result": "passed", "signature_valid": True,
            "message_id": message.message_id.hex(), "content_bytes": len(message.content),
            "content_sha256": hashlib.sha256(message.content).hexdigest(),
            "receiver": "pinned LXMRouter delivery callback",
        }

        report["stage"] = "python_to_c"
        receiver = subprocess.Popen(
            [str(args.nomad_chat), "receive-udp", str(c_identity_path),
             python_identity.get_public_key().hex(), str(c_port),
             str(max(1, int((deadline - time.monotonic()) * 1000)))],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        outgoing = LXMF.LXMessage(c_destination, python_destination, python_text,
                                  desired_method=LXMF.LXMessage.OPPORTUNISTIC)
        outgoing.pack()
        require(outgoing.method == LXMF.LXMessage.OPPORTUNISTIC,
                "python_selected_nonopportunistic_method")
        require(outgoing.representation == LXMF.LXMessage.PACKET,
                "python_selected_nonpacket_representation")
        outgoing.send()
        # C's CLI has no ready signal. Retry the same signed LXMF representation
        # with fresh encrypted packets while waiting for bind, without assuming
        # a timing-dependent startup sleep. Uses the real Python Packet sender.
        while receiver.poll() is None and time.monotonic() < deadline:
            try:
                stdout, stderr = receiver.communicate(timeout=0.15)
                break
            except subprocess.TimeoutExpired:
                RNS.Packet(c_destination, outgoing.packed[LXMF.LXMessage.DESTINATION_LENGTH:]).send()
        else:
            require(receiver.poll() is not None, "c_receive_timeout")
            stdout, stderr = receiver.communicate(timeout=1)
        check_sanitizers(stderr)
        require(receiver.returncode == 0, "c_receive_or_signature_failed")
        expected = (f"from: {python_destination.hash.hex()}\ncontent: {python_text}\n").encode()
        require(stdout == expected, "c_source_or_content_mismatch")
        report["python_to_c"] = {
            "result": "passed", "signature_valid": True,
            "message_id": outgoing.message_id.hex(), "content_bytes": len(outgoing.content),
            "content_sha256": hashlib.sha256(outgoing.content).hexdigest(),
            "receiver": "C receive-udp verified decoder",
        }
        report["result"] = "passed"
        report["stage"] = "complete"
    except VerificationFailure as error:
        report["failure_code"] = str(error)
    except Exception as error:  # No raw exception/log may disclose payloads.
        report["failure_code"] = type(error).__name__
    finally:
        if receiver is not None and receiver.poll() is None:
            receiver.kill()
            receiver.communicate(timeout=2)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if reticulum is not None:
            RNS.Reticulum.exit_handler()
    return 0 if report["result"] == "passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reticulum", type=Path, required=True)
    parser.add_argument("--lxmf", type=Path, required=True)
    parser.add_argument("--nomad-chat", type=Path, required=True)
    parser.add_argument("--rnid", type=Path, required=True)
    parser.add_argument("--native-commit", required=True,
                        help="Full source commit used to build the C binaries (caller-declared)")
    parser.add_argument("--timeout", type=float, default=30,
                        help="Worker deadline in seconds, 5 through 120")
    parser.add_argument("--output", type=Path, help="Optional privacy-safe JSON report")
    parser.add_argument("--work", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    require(5 <= args.timeout <= 120, "invalid_timeout")
    require(re.fullmatch(r"[0-9a-f]{40}", args.native_commit) is not None,
            "invalid_native_commit")
    for key in ("reticulum", "lxmf", "nomad_chat", "rnid"):
        setattr(args, key, getattr(args, key).resolve(strict=True))
    if args.work is not None:
        return worker(args)
    pinned_checkout(args.reticulum, RNS_COMMIT)
    pinned_checkout(args.lxmf, LXMF_COMMIT)
    report = {
        "schema": 1, "result": "failed", "transport": "loopback UDP",
        "scope": "bidirectional short opportunistic packets; preprovisioned public identities; no ratchets or stamps",
        "upstream_commits": {"reticulum": RNS_COMMIT, "lxmf": LXMF_COMMIT},
        "native_source_commit_declared": args.native_commit,
        "binary_sha256": {"nomad_chat": fingerprint(args.nomad_chat), "rnid": fingerprint(args.rnid)},
        "not_covered": ["announces", "router scheduling", "receipts", "direct links", "resources",
                        "ratchets", "stamps", "tickets", "propagation", "TCP", "NomadNet TUI"],
    }
    with tempfile.TemporaryDirectory(prefix="rns-lxmf-live-") as temporary:
        process = subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve()),
             "--reticulum", str(args.reticulum), "--lxmf", str(args.lxmf),
             "--nomad-chat", str(args.nomad_chat), "--rnid", str(args.rnid),
             "--native-commit", args.native_commit,
             "--timeout", str(args.timeout), "--work", temporary],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        try:
            process.wait(timeout=args.timeout + 10)
        except subprocess.TimeoutExpired:
            report["failure_code"] = "worker_timeout"
        finally:
            # Also runs for Ctrl-C; do not leave upstream or C receiver children
            # running when the enclosing temporary directory is removed.
            stop_process_group(process)
        result_path = Path(temporary) / "result.json"
        if result_path.exists():
            report.update(json.loads(result_path.read_text(encoding="utf-8")))
        else:
            report.setdefault("failure_code", "worker_failed_before_report")
        if process.returncode != 0:
            report["result"] = "failed"
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        # Exclusive creation avoids replacing an unrelated report or file.
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(encoded)
    print(encoded, end="")
    return 0 if report["result"] == "passed" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except VerificationFailure as error:
        print(json.dumps({"result": "failed", "failure_code": str(error)}))
        raise SystemExit(1) from None
