#!/usr/bin/env python3
"""Decode the synthetic C address QR using independent zxing-cpp 2.3.0.

Test-only requirements: zxing-cpp==2.3.0, numpy==1.26.4.
Pass the path to the built test_tui_qr executable. No device input is used.
"""
import importlib.metadata
import json
import subprocess
import sys

import numpy
import zxingcpp

assert importlib.metadata.version("zxing-cpp") == "2.3.0"
assert numpy.__version__ == "1.26.4"
pbm = subprocess.check_output([sys.argv[1], "--pbm"], text=True).split()
assert pbm[:3] == ["P1", "33", "33"] and len(pbm) == 1092
matrix = numpy.asarray([int(value) for value in pbm[3:]], dtype=numpy.uint8).reshape(33, 33)
assert numpy.isin(matrix, [0, 1]).all()
image = numpy.repeat(numpy.repeat((1 - matrix) * 255, 8, axis=0), 8, axis=1)
decoded = zxingcpp.read_barcode(image)
assert decoded is not None and decoded.text == "0123456789abcdef0123456789abcdef"
print(json.dumps({"ok": True, "decoder": "zxing-cpp 2.3.0", "synthetic_address": True}))
