#!/usr/bin/env python3
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


PROGRAM = """\
шифр 419999 зс5^
лен 31(е50ид)^
eeв1а3
*name arfa e50 id
*no list
*assembler
 program: ,name,
\t  ,xta , h31
\t  ,*50 , 105b
\t  ,atx , gotid
\t  ,xta , att32
\t  ,*50 , 131b
\t  ,atx , attres
\t  ,call, stop*
 h31:\t  ,oct , 0000 0000 0031 0000
 gotid:\t  ,log , 0
 attres:\t  ,log , 0
 att32:\t  ,oct , 3277 0116 1020 0356
\t  ,end ,
*execute
*end file
``````
еконец
"""


def trace_excerpt(trace):
    pieces = []
    for m in re.finditer(r"\*50 (105|131)\b", trace):
        start = max(0, m.start() - 400)
        end = min(len(trace), m.end() + 700)
        pieces.append(trace[start:end])
    if pieces:
        return "\n--- trace excerpt ---\n".join(pieces)
    return trace[:4000] + ("\n... <truncated> ..." if len(trace) > 4000 else "")


def fail(message, stdout="", stderr=""):
    print("FATAL:", message)
    if stdout:
        print("--- stdout ---")
        print(stdout)
    if stderr:
        print("--- stderr ---")
        print(trace_excerpt(stderr))
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        fail("usage: arfa-e50-id.py /path/to/dispak")
    app = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="dispak-arfa-e50-") as tmp:
        tmp = Path(tmp)
        home = tmp / "home"
        arfa = tmp / "arfa"
        src = tmp / "arfa-e50-id.b6"
        home.mkdir()
        arfa.mkdir()
        src.write_text(PROGRAM, encoding="utf-8")

        env = os.environ.copy()
        env["HOME"] = str(home)
        mkarfa = Path(__file__).resolve().parent.parent / "mkarfa.py"
        mkproc = subprocess.run(
            [sys.executable, str(mkarfa), "--arfa-dir", str(arfa),
             "--owner", "419999", "--length", "1", "е50ид"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
        )
        if mkproc.returncode != 0:
            fail("mkarfa.py failed with status %d" % mkproc.returncode,
                 mkproc.stdout, mkproc.stderr)

        cmd = [app, "-t", "-t", "--arfa-dir=" + str(arfa), str(src)]
        proc = subprocess.run(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
        )
        if proc.returncode != 0:
            fail("dispak failed with status %d" % proc.returncode,
                 proc.stdout, proc.stderr)

        trace = proc.stderr
        if "disk_open: bad diskno" in trace:
            fail("Э50 0131 fell through to ordinary disk_open",
                 proc.stdout, trace)

        if not re.search(r"\*50 105\b.*acc=0000000000310000", trace):
            fail("Э50 105 was not called for LUN 031", proc.stdout, trace)
        if not re.search(r"\*50 105\b.*\n[0-7]{5}: atx\s+\d+.*"
                         r"acc=0000011610000356", trace):
            fail("Э50 105 did not return encoded ARFA id 11610000356",
                 proc.stdout, trace)
        if not re.search(r"\*50 131\b.*acc=3277011610200356", trace):
            fail("Э50 131 was not called with encoded ARFA id",
                 proc.stdout, trace)
        if not re.search(r"\*50 131\b.*\n[0-7]{5}: atx\s+\d+.*"
                         r"acc=0000000000000000", trace):
            fail("Э50 131 did not return success", proc.stdout, trace)


if __name__ == "__main__":
    main()
