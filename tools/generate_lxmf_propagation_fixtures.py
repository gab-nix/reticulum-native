#!/usr/bin/env python3
"""Execute pinned LXMF propagation producers with synthetic, non-private data.

This is offline serialization evidence, not a transport/session test. The
upload/rejection expressions are executed from the pinned AST because neither
producer offers a side-effect-free public encoder.
"""
import argparse
import ast
import hashlib
import importlib
import json
import pathlib
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

RNS_COMMIT = "ea98db4f53dcf0defc0e71a16e60d28b1229c4e6"
LXMF_COMMIT = "795fdaa2b0777c13033787d933d1afc94a2377cb"


def checked_head(path, expected):
    actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if actual != expected:
        raise SystemExit(f"expected {expected}, found {actual}: {path}")


def assignment_expression(path, target):
    source = path.read_text(encoding="utf-8")
    tree = ast.parse(source)
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call):
            if any(ast.unparse(t) == target for t in node.targets):
                return compile(ast.Expression(node.value), str(path), "eval"), ast.unparse(node.value)
    raise ValueError(f"missing upstream assignment: {target}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--reticulum", required=True, type=pathlib.Path)
    p.add_argument("--lxmf", required=True, type=pathlib.Path)
    p.add_argument("--output-dir", required=True, type=pathlib.Path)
    args = p.parse_args()
    checked_head(args.reticulum, RNS_COMMIT)
    checked_head(args.lxmf, LXMF_COMMIT)
    sys.path[:0] = [str(args.reticulum), str(args.lxmf)]
    import RNS
    import LXMF
    import RNS.vendor.umsgpack as msgpack
    from LXMF.LXMRouter import LXMRouter
    from LXMF.LXMPeer import LXMPeer
    from LXMF.LXMF import pn_announce_data_is_valid
    for module, root in ((RNS, args.reticulum), (LXMF, args.lxmf)):
        if not pathlib.Path(module.__file__).resolve().is_relative_to(root.resolve()):
            raise SystemExit("module imported outside pinned checkout")
    router_module = importlib.import_module("LXMF.LXMRouter")
    fixtures = []
    def add(name, kind, wire):
        fixtures.append((name, kind, wire))

    fake = SimpleNamespace(name="Rei PN 🌸", propagation_node=True, from_static_only=False,
        propagation_stamp_cost=16, propagation_stamp_cost_flexibility=3, peering_cost=18,
        propagation_per_transfer_limit=256, propagation_per_sync_limit=10240)
    fake.get_propagation_node_announce_metadata = lambda: LXMRouter.get_propagation_node_announce_metadata(fake)
    with patch.object(router_module.time, "time", return_value=1700000000.25):
        wire = LXMRouter.get_propagation_node_app_data(fake)
        assert pn_announce_data_is_valid(wire)
        add("announce_enabled", 0, wire)
        fake.from_static_only = True
        fake.propagation_per_transfer_limit = 256.5
        fake.propagation_per_sync_limit = 10240.25
        add("announce_static_float_limits", 0, LXMRouter.get_propagation_node_app_data(fake))
    extended = msgpack.unpackb(wire)
    extended[6][255] = {"future": [True, b"opaque"]}
    extended.append({3: b"extension"})
    extended_wire = msgpack.packb(extended)
    assert pn_announce_data_is_valid(extended_wire)
    add("announce_unknown_fields", 0, extended_wire)

    synthetic = b"D" * 16 + b"encrypted-fixture-not-a-real-message" * 3
    upload_expr, upload_source = assignment_expression(args.lxmf / "LXMF/LXMessage.py", "self.propagation_packed")
    add("upload", 1, eval(upload_expr, {"msgpack": msgpack, "time": SimpleNamespace(time=lambda: 1700000000.25), "lxmf_data": synthetic + b"S" * 32}))
    reject_expr, reject_source = assignment_expression(args.lxmf / "LXMF/LXMRouter.py", "reject_data")
    add("upload_invalid_stamp", 5, eval(reject_expr, {"msgpack": msgpack, "LXMPeer": LXMPeer}))
    # List request shape is the actual argument of request_messages_from_propagation_node.
    tree = ast.parse((args.lxmf / "LXMF/LXMRouter.py").read_text())
    list_data = None
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == "request":
            if len(node.args) >= 2 and ast.unparse(node.args[0]) == "LXMPeer.MESSAGE_GET_PATH" and ast.unparse(node.args[1]) == "[None, None]":
                list_data = ast.literal_eval(node.args[1])
    assert list_data == [None, None]
    add("list_request", 2, msgpack.packb(list_data))
    ids = [bytes(range(32)), bytes(range(32, 64))]
    recorded = []
    link = SimpleNamespace(request=lambda path, data, **kw: recorded.append((path, data)))
    client = SimpleNamespace(outbound_propagation_link=link, has_message=lambda ident: ident == ids[1],
        retain_synced_on_node=False, propagation_transfer_max_messages=LXMRouter.PR_ALL_MESSAGES,
        delivery_per_transfer_limit=1000, message_get_response=lambda r: None,
        message_get_failed=lambda r: None, message_get_progress=lambda r: None)
    LXMRouter.message_list_response(client, SimpleNamespace(response=ids, link=link))
    assert recorded[-1][0] == "/get"
    add("download_request", 2, msgpack.packb(recorded[-1][1]))
    client.lxmf_propagation = lambda data, **kw: True
    client.save_locally_delivered_transient_ids = lambda: None
    LXMRouter.message_get_response(client, SimpleNamespace(response=[synthetic], link=link))
    add("acknowledge_request", 2, msgpack.packb(recorded[-1][1]))
    identity = RNS.Identity.from_bytes(bytes(range(64)))
    remote = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")
    server = SimpleNamespace(identity_allowed=lambda _: True, propagation_entries={}, client_propagation_messages_served=0)
    with tempfile.TemporaryDirectory() as directory:
        for index, transient in enumerate(ids):
            filename = pathlib.Path(directory) / str(index)
            filename.write_bytes(synthetic + bytes([index]) + b"S" * 32)
            server.propagation_entries[transient] = [remote.hash, str(filename)]
        invoke = lambda data, ident=identity: LXMRouter.message_get_request(server, "/get", data, b"R" * 16, ident, 1700000000.25)
        add("list_response", 3, msgpack.packb(invoke([None, None])))
        add("download_response", 4, msgpack.packb(invoke([ids, [], 1000])))
        add("download_limit_empty", 4, msgpack.packb(invoke([ids, [], 0])))
        add("missing_identity", 3, msgpack.packb(invoke([None, None], None)))
        server.identity_allowed = lambda _: False
        add("access_denied", 4, msgpack.packb(invoke([None, None])))

    out = ["/* Generated by tools/generate_lxmf_propagation_fixtures.py. Synthetic data only.",
        f" * RNS {RNS_COMMIT}; LXMF {LXMF_COMMIT}. */",
        "#ifndef LXMF_PYTHON_PROPAGATION_FIXTURES_H", "#define LXMF_PYTHON_PROPAGATION_FIXTURES_H",
        "#include <stddef.h>", "#include <stdint.h>",
        "typedef struct { const char *name; unsigned kind; const uint8_t *wire; size_t length; } pn_fixture;"]
    for index, (_, _, data) in enumerate(fixtures):
        out.append(f"static const uint8_t pn_wire_{index}[] = {{" + ",".join(f"0x{b:02x}" for b in data) + "};")
    out.append("static const pn_fixture pn_fixtures[] = {")
    for index, (name, kind, data) in enumerate(fixtures):
        out.append(f'    {{"{name}", {kind}, pn_wire_{index}, {len(data)}}},')
    out += ["};", "#endif", ""]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "lxmf_propagation_vectors.h").write_text("\n".join(out))
    provenance = {"schema": 1, "reticulum_commit": RNS_COMMIT, "lxmf_commit": LXMF_COMMIT,
        "generator": "tools/generate_lxmf_propagation_fixtures.py",
        "evidence": "Offline upstream serialization only; not network interoperability",
        "source_apis": ["LXMRouter.get_propagation_node_app_data", "LXMRouter.message_list_response",
            "LXMRouter.message_get_response", "LXMRouter.message_get_request"],
        "source_expressions": {"upload": upload_source, "rejection": reject_source},
        "fixtures": [{"name": n, "bytes": len(d), "sha256": hashlib.sha256(d).hexdigest()} for n, _, d in fixtures]}
    (args.output_dir / "lxmf_propagation.provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")


if __name__ == "__main__":
    main()
