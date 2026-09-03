#!/usr/bin/env python3
"""Generate public RRC envelope vectors from pinned NomadNet 1.2.0."""

import argparse
import importlib.util
import pathlib
import subprocess
import sys
import types

NOMADNET_COMMIT = "475c0ee2a0388cf8470e7f1e90d5decb67b579ea"


def checked_head(repository: pathlib.Path) -> None:
    actual = subprocess.check_output(
        ["git", "-C", str(repository), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != NOMADNET_COMMIT:
        raise SystemExit(
            f"{repository}: expected commit {NOMADNET_COMMIT}, found {actual}"
        )


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def load_rrc(nomadnet: pathlib.Path):
    package_root = nomadnet / "nomadnet"
    package = types.ModuleType("nomadnet")
    package.__path__ = [str(package_root)]
    vendor = types.ModuleType("nomadnet.vendor")
    vendor.__path__ = [str(package_root / "vendor")]
    sys.modules["nomadnet"] = package
    sys.modules["nomadnet.vendor"] = vendor
    cbor = load_module("nomadnet.vendor.cbor", package_root / "vendor" / "cbor.py")
    vendor.cbor = cbor

    # RRC.py only uses RNS inside methods that perform live networking. A tiny
    # empty module lets us execute its actual constants and _make_envelope()
    # without importing Reticulum or NomadNet's unavailable UI dependencies.
    sys.modules["RNS"] = types.ModuleType("RNS")
    rrc = load_module("nomadnet.RRC", package_root / "RRC.py")
    return rrc, cbor


def c_bytes(data: bytes) -> str:
    return ", ".join(f"0x{value:02x}" for value in data)


def byte_array(lines: list[str], name: str, data: bytes) -> str:
    if not data:
        return "NULL"
    lines.append(f"static const uint8_t {name}[] = {{{c_bytes(data)}}};")
    return name


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nomadnet", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    checked_head(args.nomadnet)
    rrc, cbor = load_rrc(args.nomadnet.resolve())

    source = bytes(range(16))
    other_source = bytes(range(16, 32))
    timestamps = [
        23, 24, 255, 256, 65535, 65536,
        4294967295, 4294967296, 18446744073709551615,
        1700000000123, 1700000000124, 1700000000125,
    ]
    descriptions = [
        (rrc.T_HELLO, None, {
            rrc.B_HELLO_NAME: "nomadnet",
            rrc.B_HELLO_VER: "0.1",
            rrc.B_HELLO_CAPS: {rrc.CAP_RESOURCE_ENVELOPE: True, 99: "future"},
        }, "れい"),
        (rrc.T_WELCOME, None, {
            rrc.B_WELCOME_HUB: "hub 🌸",
            rrc.B_WELCOME_VER: "1.2.0",
            rrc.B_WELCOME_CAPS: {rrc.CAP_RESOURCE_ENVELOPE: True, 42: False},
            rrc.B_WELCOME_LIMITS: {
                rrc.L_MAX_NICK_BYTES: 32,
                rrc.L_MAX_ROOM_NAME_BYTES: 64,
                rrc.L_MAX_MSG_BODY_BYTES: 350,
                rrc.L_MAX_ROOMS_PER_SESSION: 32,
                rrc.L_RATE_LIMIT_MSGS_PER_MINUTE: 240,
                99: 65536,
            },
        }, None),
        (rrc.T_JOIN, "lobby-π", "shared key", "Rei"),
        (rrc.T_JOINED, "lobby", [source, other_source], None),
        (rrc.T_PART, "lobby", None, None),
        (rrc.T_PARTED, "lobby", [other_source], None),
        (rrc.T_MSG, "lobby", "Hello, 世界 🌸", "Rei"),
        (rrc.T_NOTICE, None, "Registered public rooms\n#lobby - General", None),
        (rrc.T_PING, None, bytes.fromhex("0102030405060708"), None),
        (rrc.T_PONG, None, bytes.fromhex("0102030405060708"), None),
        (rrc.T_ERROR, "locked", "Access denied", None),
        (rrc.T_RESOURCE_ENVELOPE, "lobby", {
            rrc.B_RES_ID: bytes.fromhex("a0a1a2a3a4a5a6a7"),
            rrc.B_RES_KIND: rrc.RES_KIND_MOTD,
            rrc.B_RES_SIZE: 65536,
            rrc.B_RES_SHA256: bytes(range(32)),
            rrc.B_RES_ENCODING: "utf-8",
            99: {0: [-1, 23, 24, 255, 256, 65535, 65536,
                     4294967295, 4294967296, 18446744073709551615],
                 1: "未来", 2: None},
        }, None),
    ]
    if len(descriptions) != 12:
        raise SystemExit("unexpected RRC type table size")

    vectors = []
    for index, ((msg_type, room, body, nick), timestamp) in enumerate(
        zip(descriptions, timestamps)
    ):
        message_id = bytes(((index * 8 + i) & 0xff) for i in range(8))
        envelope = rrc._make_envelope(  # pylint: disable=protected-access
            msg_type, source, room=room, body=body, nick=nick,
            mid=message_id, ts=timestamp
        )
        canonical = cbor.encode(envelope)
        body_cbor = cbor.encode(body) if body is not None else b""
        wire_envelope = dict(envelope)
        has_unknown_outer = msg_type == rrc.T_ERROR
        if has_unknown_outer:
            wire_envelope[255] = {0: "future", 1: [False, None, -1]}
        wire = cbor.encode(wire_envelope)
        if cbor.decode(wire) != wire_envelope:
            raise SystemExit("pinned CBOR encoder did not round-trip a vector")
        vectors.append({
            "type": msg_type,
            "message_id": message_id,
            "timestamp": timestamp,
            "source": source,
            "room": room.encode("utf-8") if room is not None else b"",
            "body": body_cbor,
            "nick": nick.encode("utf-8") if nick is not None else b"",
            "wire": wire,
            "canonical": canonical,
            "unknown_outer": has_unknown_outer,
        })

    lines = [
        "/* Generated by tools/generate_nomadnet_rrc_fixtures.py.",
        f" * NomadNet commit: {NOMADNET_COMMIT}",
        " * Source APIs: nomadnet.RRC._make_envelope and",
        " * nomadnet.vendor.cbor.encode/decode.",
        " * Contains public synthetic test material only. Do not edit manually. */",
        "#ifndef RETICULUM_TEST_NOMADNET_RRC_FIXTURES_H",
        "#define RETICULUM_TEST_NOMADNET_RRC_FIXTURES_H",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "typedef struct {",
        "    uint8_t type; const uint8_t *message_id; uint64_t timestamp_ms;",
        "    const uint8_t *source; const uint8_t *room; size_t room_len;",
        "    const uint8_t *body; size_t body_len;",
        "    const uint8_t *nick; size_t nick_len;",
        "    const uint8_t *wire; size_t wire_len;",
        "    const uint8_t *canonical; size_t canonical_len;",
        "    uint8_t has_unknown_outer;",
        "} nomadnet_rrc_fixture;",
    ]
    references = []
    for index, vector in enumerate(vectors):
        refs = {}
        for key in ("message_id", "source", "room", "body", "nick", "wire", "canonical"):
            refs[key] = byte_array(lines, f"rrc_fixture_{index}_{key}", vector[key])
        references.append(refs)
    lines.append("static const nomadnet_rrc_fixture nomadnet_rrc_fixtures[] = {")
    for vector, refs in zip(vectors, references):
        lines.extend([
            "    {",
            f"        {vector['type']}u, {refs['message_id']}, UINT64_C({vector['timestamp']}),",
            f"        {refs['source']}, {refs['room']}, {len(vector['room'])}u,",
            f"        {refs['body']}, {len(vector['body'])}u, {refs['nick']}, {len(vector['nick'])}u,",
            f"        {refs['wire']}, {len(vector['wire'])}u,",
            f"        {refs['canonical']}, {len(vector['canonical'])}u,",
            f"        {1 if vector['unknown_outer'] else 0}u",
            "    },",
        ])
    lines.extend([
        "};",
        "#define NOMADNET_RRC_FIXTURE_COUNT \\",
        "    (sizeof nomadnet_rrc_fixtures / sizeof nomadnet_rrc_fixtures[0])",
        "#endif",
        "",
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
