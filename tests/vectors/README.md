# Upstream compatibility vectors

Vectors in the unit tests are generated from the pinned revisions listed in
`docs/COMPATIBILITY.md`. The first LXMF payload vector is produced with:

```sh
PYTHONPATH=/path/to/Reticulum python3 -c \
  'import RNS.vendor.umsgpack as m; print(m.packb([1.5,b"hi",b"hello",{1:b"x"}]).hex())'
```

Expected LXMF 1.1.0 bytes:

```text
94cb3ff8000000000000c4026869c40568656c6c6f8101c40178
```

Every new vector must record its upstream release or commit, generator inputs,
and exact expected bytes. Secret or randomly generated values must never be
committed as fixtures.
