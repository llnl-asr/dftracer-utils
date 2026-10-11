#!/usr/bin/env python3
"""Delete the PyPI prereleases <version>.postN.devX of one release but the newest.

PyPI has no API to delete a release, so this logs in to the website through
pypi-cleanup (https://github.com/arcivanov/pypi-cleanup). The account must own
the project and have TOTP two-factor authentication.

Modes: --query-only lists the plan with no login. The default also logs in and
has pypi-cleanup list its own plan, but deletes nothing. --do-it deletes.

Environment for a login: PYPI_CLEANUP_USERNAME, PYPI_CLEANUP_PASSWORD and
PYPI_CLEANUP_TOTP_SECRET (the base32 secret of the TOTP device).
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request


def plan(versions, version):
    """Return (keep, delete) among the <version>.postN.devX of versions.

    keep is the one with the highest (N, X), or None when there is none.
    """
    pattern = re.compile(rf"{re.escape(version)}\.post(\d+)\.dev(\d+)")
    found = []
    for v in versions:
        m = pattern.fullmatch(v)
        if m:
            found.append(((int(m.group(1)), int(m.group(2))), v))
    ordered = [v for _, v in sorted(found)]
    if not ordered:
        return None, []
    return ordered[-1], ordered[:-1]


def delete_regex(delete):
    return "^(" + "|".join(re.escape(v) for v in delete) + ")$"


def pypi_versions(host, package):
    request = urllib.request.Request(
        f"{host}/simple/{package}/",
        headers={"Accept": "application/vnd.pypi.simple.v1+json"},
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)["versions"]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--package", required=True)
    parser.add_argument(
        "--version", required=True, help="release whose prereleases go, e.g. 0.0.14"
    )
    parser.add_argument("--host", default="https://pypi.org")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--query-only", action="store_true")
    mode.add_argument("--do-it", action="store_true")
    args = parser.parse_args()
    host = args.host.rstrip("/")

    keep, delete = plan(pypi_versions(host, args.package), args.version)
    if keep is None:
        print(f"no {args.version}.postN.devX prereleases on {host}")
        return 0
    print(f"keep {keep}")
    for v in delete:
        print(f"delete {v}")
    if not delete or args.query_only:
        return 0

    tool = shutil.which("pypi-cleanup")
    if tool is None:
        sys.exit("pypi-cleanup is not on PATH")
    for name in ("PYPI_CLEANUP_USERNAME", "PYPI_CLEANUP_PASSWORD", "PYPI_CLEANUP_TOTP_SECRET"):
        if not os.environ.get(name):
            sys.exit(f"{name} is not set")

    import pyotp

    cmd = [
        tool,
        "-t",
        host + "/",
        "-p",
        args.package,
        "-r",
        delete_regex(delete),
        "-u",
        os.environ["PYPI_CLEANUP_USERNAME"],
    ]
    if args.do_it:
        cmd.append("--do-it")
    # pypi-cleanup reads the two-factor code from stdin after the password login.
    code = pyotp.TOTP(os.environ["PYPI_CLEANUP_TOTP_SECRET"]).now()
    result = subprocess.run(cmd, input=code + "\n", text=True, check=False)
    if result.returncode != 0 or not args.do_it:
        return result.returncode

    # The simple index sits behind a CDN, so give a delete time to show.
    for _ in range(10):
        left, gone = plan(pypi_versions(host, args.package), args.version)
        if left == keep and not gone:
            return 0
        time.sleep(30)
    sys.exit(f"after the delete {host} still lists {gone + [left]}, expected only {keep}")


if __name__ == "__main__":
    sys.exit(main())
