"""Starts the Spinus AI service: python run_spinus_ai.py [--log FILE] <spinus_ai arguments>.

Spinus runs this file (not `python -m spinus_ai`) so Python finds the spinus_ai package beside it, on Linux
and Windows alike, without PYTHONPATH. --log sends the service's errors to FILE, before anything is
imported, so a missing package is reported there too.
"""

import sys

if len(sys.argv) > 2 and sys.argv[1] == "--log":
    sys.stderr = open(sys.argv[2], "a", encoding="utf-8", buffering=1)  # noqa: SIM115 (open for the whole run)
    del sys.argv[1:3]

from spinus_ai.__main__ import main  # after the redirect

main()
