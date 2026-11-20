"""Minimal Qt fetcher for the new download.qt.io layout (qt6_XYZ/qt6_XYZ_<arch>/).

aqtinstall 3.3.0 still looks for qt6_XYZ/Updates.xml, which now returns 404.
This script downloads the same official archives from download.qt.io, checks
their SHA1 against the repository's .sha1 files, and extracts them with py7zr.

Usage: python qt_fetch.py <version e.g. 6.12.1> <arch e.g. msvc2022_64> <outdir> [addon ...]
"""
import hashlib
import re
import ssl
import sys
import urllib.request
from pathlib import Path

import certifi
import py7zr

BASE = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"
SKIP = ("qtdoc-",)  # documentation, not needed for building


SSL_CTX = ssl.create_default_context(cafile=certifi.where())


def get(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=300, context=SSL_CTX) as r:
        return r.read()


def main() -> None:
    ver, arch, outdir, *addons = sys.argv[1:]
    v = ver.replace(".", "")
    folder = f"{BASE}/qt6_{v}/qt6_{v}_{arch}"
    xml = get(f"{folder}/Updates.xml").decode("utf-8")
    wanted = {f"qt.qt6.{v}.win64_{arch}"} | {f"qt.qt6.{v}.addons.{a}.win64_{arch}" for a in addons}

    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    tmp = out / "_dl"
    tmp.mkdir(exist_ok=True)

    found = set()
    for pkg in re.findall(r"<PackageUpdate>(.*?)</PackageUpdate>", xml, re.S):
        name = re.search(r"<Name>(.*?)</Name>", pkg).group(1)
        if name not in wanted:
            continue
        found.add(name)
        version = re.search(r"<Version>(.*?)</Version>", pkg).group(1)
        archives = re.search(r"<DownloadableArchives>(.*?)</DownloadableArchives>", pkg, re.S).group(1)
        for arc in (a.strip() for a in archives.split(",") if a.strip()):
            if arc.startswith(SKIP):
                continue
            url = f"{folder}/{name}/{version}{arc}"
            expected = get(url + ".sha1").decode().split()[0].strip().lower()
            dest = tmp / arc
            print(f"indiriliyor: {arc}", flush=True)
            data = get(url)
            actual = hashlib.sha1(data).hexdigest()
            if actual != expected:
                sys.exit(f"SHA1 UYUSMAZLIGI: {arc} beklenen={expected} gelen={actual}")
            dest.write_bytes(data)
            with py7zr.SevenZipFile(dest, "r") as z:
                z.extractall(out)
            dest.unlink()

    missing = wanted - found
    if missing:
        sys.exit(f"Bulunamayan paketler: {sorted(missing)}")
    tmp.rmdir()
    print("tamam")


if __name__ == "__main__":
    main()
