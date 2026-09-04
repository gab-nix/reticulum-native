#!/usr/bin/env python3
"""Opt-in C client to pinned-Python RRC schema fixture-hub acceptance test.

NomadNet 1.2.0 does not ship an RRC server. This harness deliberately uses
its unmodified RRC envelope builder and vendored CBOR codec on a pinned RNS
inbound destination; it must not be described as stock rrcd verification.
"""

import argparse
import datetime
import hashlib
import importlib.util
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import types

RNS_COMMIT = "ea98db4f53dcf0defc0e71a16e60d28b1229c4e6"
NOMADNET_COMMIT = "475c0ee2a0388cf8470e7f1e90d5decb67b579ea"


def checked_checkout(path, commit):
    if (path / ".git").exists():
        actual = subprocess.check_output(
            ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
        ).strip()
        if actual != commit:
            raise RuntimeError(f"Pinned revision mismatch: {path}")
        dirty = subprocess.check_output(
            ["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"],
            text=True,
        ).strip()
        if dirty:
            raise RuntimeError(f"Pinned source has tracked modifications: {path}")
        return "git checkout"
    marker = path.parent / f"{path.name}.provenance"
    if not marker.is_file():
        raise RuntimeError(f"Missing pinned-source provenance: {path}")
    fields = dict(
        line.split("=", 1) for line in marker.read_text().splitlines() if "=" in line
    )
    if fields.get("commit") != commit or fields.get("tracked_status") != "clean":
        raise RuntimeError(f"Invalid pinned-source provenance: {marker}")
    return "recorded clean source tree"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load pinned module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def load_rrc(nomadnet, rns):
    root = nomadnet / "nomadnet"
    package = types.ModuleType("nomadnet")
    package.__path__ = [str(root)]
    vendor = types.ModuleType("nomadnet.vendor")
    vendor.__path__ = [str(root / "vendor")]
    sys.modules["nomadnet"] = package
    sys.modules["nomadnet.vendor"] = vendor
    sys.modules["RNS"] = rns
    cbor = load_module("nomadnet.vendor.cbor", root / "vendor" / "cbor.py")
    vendor.cbor = cbor
    return load_module("nomadnet.RRC", root / "RRC.py"), cbor


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reticulum", required=True, type=pathlib.Path)
    parser.add_argument("--nomadnet", required=True, type=pathlib.Path)
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    rns_source_evidence = checked_checkout(args.reticulum, RNS_COMMIT)
    nomadnet_source_evidence = checked_checkout(args.nomadnet, NOMADNET_COMMIT)
    sys.path.insert(0, str(args.reticulum.resolve()))
    import RNS  # pylint: disable=import-outside-toplevel

    if not pathlib.Path(RNS.__file__).resolve().is_relative_to(
        args.reticulum.resolve()
    ):
        raise RuntimeError("Imported RNS outside pinned checkout")
    rrc, cbor = load_rrc(args.nomadnet.resolve(), RNS)
    report = {
        "schema": 1,
        "rns_commit": RNS_COMMIT,
        "rns_version": RNS.__version__,
        "nomadnet_commit": NOMADNET_COMMIT,
        "nomadnet_version": "1.2.0",
        "rns_source_evidence": rns_source_evidence,
        "nomadnet_source_evidence": nomadnet_source_evidence,
        "endpoint": "pinned RNS fixture hub using NomadNet RRC schema APIs",
        "transport": "loopback UDP",
        "identified": False,
        "client_fields_valid": False,
        "hello_valid": False,
        "join_valid": False,
        "message_valid": False,
        "ping_valid": False,
        "part_valid": False,
        "driver": {},
        "errors": [],
        "ok": False,
        "limitations": [
            "NomadNet 1.2.0 contains an RRC client but no stock hub server",
            "fixture hub is not a live rrcd deployment",
            "no Resource envelopes, reconnect, relay hop or TCP coverage",
        ],
    }
    report["run_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    repository = pathlib.Path(__file__).resolve().parents[1]
    report["native_base_revision"] = subprocess.check_output(
        ["git", "-C", str(repository), "rev-parse", "HEAD"], text=True
    ).strip()
    report["driver_source_sha256"] = hashlib.sha256(
        (repository / "tests" / "interop" / "rrc_live_driver.c").read_bytes()
    ).hexdigest()

    with tempfile.TemporaryDirectory(prefix="rrc-live-") as temporary:
        root = pathlib.Path(temporary)
        c_port, python_port = reserve_port(), reserve_port()
        while c_port == python_port:
            python_port = reserve_port()
        config = root / "rns"
        config.mkdir()
        (config / "config").write_text(
            "[reticulum]\nshare_instance = No\nenable_transport = No\n"
            "[logging]\nloglevel = 0\n[interfaces]\n[[RRC fixture]]\n"
            "type = UDPInterface\ninterface_enabled = True\n"
            f"listen_ip = 127.0.0.1\nlisten_port = {python_port}\n"
            f"forward_ip = 127.0.0.1\nforward_port = {c_port}\n"
        )
        reticulum = RNS.Reticulum(configdir=str(config), loglevel=0)
        hub_identity = RNS.Identity()
        destination = RNS.Destination(
            hub_identity,
            RNS.Destination.IN,
            RNS.Destination.SINGLE,
            "rrc",
            "hub",
        )
        state = {"link": None, "remote": None, "message_id": None}

        def send(message_type, room=None, body=None, nick=None):
            envelope = rrc._make_envelope(  # pylint: disable=protected-access
                message_type,
                hub_identity.hash,
                room=room,
                body=body,
                nick=nick,
            )
            RNS.Packet(state["link"], cbor.encode(envelope)).send()

        def common_valid(envelope, expected_keys):
            remote = state["remote"]
            now_ms = int(time.time() * 1000)
            return (
                remote is not None
                and set(envelope.keys()) == expected_keys
                and envelope.get(rrc.K_V) == rrc.RRC_VERSION
                and isinstance(envelope.get(rrc.K_ID), bytes)
                and len(envelope[rrc.K_ID]) == 8
                and isinstance(envelope.get(rrc.K_TS), int)
                and abs(now_ms - envelope[rrc.K_TS]) < 30000
                and envelope.get(rrc.K_SRC) == remote.hash
            )

        def packet_received(data, _packet):
            try:
                envelope = cbor.decode(data)
                if not isinstance(envelope, dict):
                    raise ValueError("not a map")
                message_type = envelope.get(rrc.K_T)
                if message_type == rrc.T_HELLO:
                    keys = {rrc.K_V, rrc.K_T, rrc.K_ID, rrc.K_TS,
                            rrc.K_SRC, rrc.K_BODY, rrc.K_NICK}
                    body = envelope.get(rrc.K_BODY)
                    common = common_valid(envelope, keys)
                    body_valid = body == {
                        rrc.B_HELLO_NAME: "nomadnet",
                        rrc.B_HELLO_VER: "0.1",
                        rrc.B_HELLO_CAPS: {rrc.CAP_RESOURCE_ENVELOPE: True},
                    }
                    nick_valid = envelope.get(rrc.K_NICK) == "Rei"
                    valid = common and body_valid and nick_valid
                    report["hello_valid"] = bool(valid)
                    report["client_fields_valid"] = bool(valid)
                    if not valid:
                        remote = state["remote"]
                        now_ms = int(time.time() * 1000)
                        raise ValueError(
                            "HELLO fields "
                            f"keys={set(envelope.keys()) == keys} "
                            f"id={isinstance(envelope.get(rrc.K_ID), bytes) and len(envelope.get(rrc.K_ID, b'')) == 8} "
                            f"timestamp={isinstance(envelope.get(rrc.K_TS), int) and abs(now_ms-envelope.get(rrc.K_TS, 0)) < 30000} "
                            f"source={remote is not None and envelope.get(rrc.K_SRC) == remote.hash} "
                            f"body={body_valid} nick={nick_valid}"
                        )
                    send(rrc.T_WELCOME, body={
                        rrc.B_WELCOME_HUB: "Pinned fixture hub",
                        rrc.B_WELCOME_VER: "NomadNet-1.2.0-schema",
                        rrc.B_WELCOME_CAPS: {rrc.CAP_RESOURCE_ENVELOPE: True},
                        rrc.B_WELCOME_LIMITS: {
                            rrc.L_MAX_NICK_BYTES: 32,
                            rrc.L_MAX_ROOM_NAME_BYTES: 64,
                            rrc.L_MAX_MSG_BODY_BYTES: 350,
                            rrc.L_MAX_ROOMS_PER_SESSION: 32,
                            rrc.L_RATE_LIMIT_MSGS_PER_MINUTE: 240,
                        },
                    })
                elif message_type == rrc.T_JOIN:
                    keys = {rrc.K_V, rrc.K_T, rrc.K_ID, rrc.K_TS, rrc.K_SRC,
                            rrc.K_ROOM, rrc.K_BODY, rrc.K_NICK}
                    valid = common_valid(envelope, keys) and (
                        envelope.get(rrc.K_ROOM) == "lobby"
                        and envelope.get(rrc.K_BODY) == "synthetic-key"
                        and envelope.get(rrc.K_NICK) == "Rei"
                    )
                    report["join_valid"] = bool(valid)
                    if not valid:
                        raise ValueError("JOIN fields")
                    send(rrc.T_JOINED, room="lobby", body=[state["remote"].hash])
                elif message_type == rrc.T_MSG:
                    keys = {rrc.K_V, rrc.K_T, rrc.K_ID, rrc.K_TS, rrc.K_SRC,
                            rrc.K_ROOM, rrc.K_BODY, rrc.K_NICK}
                    valid = common_valid(envelope, keys) and (
                        envelope.get(rrc.K_ROOM) == "lobby"
                        and envelope.get(rrc.K_BODY) == "c-to-python"
                        and envelope.get(rrc.K_NICK) == "Rei"
                    )
                    report["message_valid"] = bool(valid)
                    state["message_id"] = envelope.get(rrc.K_ID)
                    if not valid:
                        raise ValueError("MSG fields")
                    send(rrc.T_MSG, room="lobby", body="python-to-c",
                         nick="PythonHub")
                elif message_type == rrc.T_PING:
                    keys = {rrc.K_V, rrc.K_T, rrc.K_ID, rrc.K_TS,
                            rrc.K_SRC, rrc.K_BODY}
                    nonce = envelope.get(rrc.K_BODY)
                    valid = common_valid(envelope, keys) and (
                        isinstance(nonce, bytes) and len(nonce) == 8
                    )
                    report["ping_valid"] = bool(valid)
                    if not valid:
                        raise ValueError("PING fields")
                    send(rrc.T_PONG, body=nonce)
                elif message_type == rrc.T_PART:
                    keys = {rrc.K_V, rrc.K_T, rrc.K_ID, rrc.K_TS,
                            rrc.K_SRC, rrc.K_ROOM}
                    valid = common_valid(envelope, keys) and (
                        envelope.get(rrc.K_ROOM) == "lobby"
                    )
                    report["part_valid"] = bool(valid)
                    if not valid:
                        raise ValueError("PART fields")
                    send(rrc.T_PARTED, room="lobby", body=[])
                else:
                    raise ValueError("unexpected type")
            except Exception as error:  # callback thread cannot raise to main
                report["errors"].append(str(error))

        def identified(link, identity):
            if link is state["link"] and identity is not None:
                state["remote"] = identity
                report["identified"] = True

        def established(link):
            state["link"] = link
            link.set_packet_callback(packet_received)
            link.set_remote_identified_callback(identified)

        destination.set_link_established_callback(established)
        destination.announce()
        command = [
            str(args.driver.resolve()),
            str(c_port),
            str(python_port),
            destination.hash.hex(),
            hub_identity.get_public_key().hex(),
        ]
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        started = time.monotonic()
        last_announce = started
        while process.poll() is None and time.monotonic() - started < 45:
            now = time.monotonic()
            if now - last_announce >= 1:
                destination.announce()
                last_announce = now
            time.sleep(0.01)
        if process.poll() is None:
            process.terminate()
        stdout, stderr = process.communicate(timeout=5)
        if stderr.strip():
            report["errors"].append("native stderr")
        try:
            report["driver"] = json.loads(stdout.strip().splitlines()[-1])
        except (IndexError, json.JSONDecodeError):
            report["errors"].append("native JSON")
        report["native_exit"] = process.returncode
        message_id = state["message_id"].hex() if state["message_id"] else None
        report["message_id_correlated"] = (
            message_id is not None
            and report["driver"].get("message_id") == message_id
        )
        report["ok"] = bool(
            process.returncode == 0
            and report["driver"].get("ok")
            and report["identified"]
            and report["client_fields_valid"]
            and report["hello_valid"]
            and report["join_valid"]
            and report["message_valid"]
            and report["ping_valid"]
            and report["part_valid"]
            and report["message_id_correlated"]
            and not report["errors"]
        )
        saved_stdout, saved_stderr = sys.stdout, sys.stderr
        reticulum.exit_handler()
        sys.stdout, sys.stderr = saved_stdout, saved_stderr

    report["driver_sha256"] = hashlib.sha256(args.driver.read_bytes()).hexdigest()
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
