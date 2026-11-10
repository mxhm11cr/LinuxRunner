#!/usr/bin/env python3
"""Contract for the native SteamOS image reader and the streamed fakefs import.

Always runs (seconds, no network):
  * GPT partition extraction from a synthetic disk image, raw and as
    multi-stream bzip2, including a partition that starts inside and ends
    beyond the reader's 4 MiB header buffer, and corrupt-GPT rejection.
  * fakefsify "-" (fakefs_import_fd): a PAX stream with directories, files,
    symlinks, hard links, setuid modes, owners and a security.capability
    attribute; a truncated stream must fail.
Optional (--image rootfs-A.img from Valve's SteamFrame recovery image,
SHA-256 668da2b1...): full export invariants measured on 2026-11-08.

usage: steamos_image_contract.py --tool steamos-image --fakefsify fakefsify [--image rootfs-A.img]
"""
import argparse, binascii, bz2, hashlib, io, json, os, sqlite3, struct, subprocess, sys, tarfile, tempfile, uuid

def gpt_image(partition: bytes, name="rootfs-A", first_lba=4096, sector=512):
    """Minimal valid GPT disk with one named partition and one decoy."""
    entries = bytearray(128 * 128)
    def entry(index, label, first, last):
        e = bytearray(128)
        e[0:16] = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
        e[16:32] = uuid.uuid4().bytes_le
        struct.pack_into("<QQ", e, 32, first, last)
        e[56:56 + 2 * len(label)] = label.encode("utf-16-le")
        entries[index * 128:(index + 1) * 128] = e
    entry(0, "rootfs-B", 34, first_lba - 1)
    last = first_lba + len(partition) // sector - 1
    entry(1, name, first_lba, last)
    header = bytearray(sector)
    header[0:8] = b"EFI PART"
    struct.pack_into("<IIII", header, 8, 0x10000, 92, 0, 0)
    struct.pack_into("<QQQQ", header, 24, 1, last + 33, 34, last)
    header[56:72] = uuid.uuid4().bytes_le
    struct.pack_into("<QIII", header, 72, 2, 128, 128, binascii.crc32(entries) & 0xffffffff)
    struct.pack_into("<I", header, 16, binascii.crc32(bytes(header[:92])) & 0xffffffff)
    disk = bytearray(first_lba * sector)
    disk[sector:2 * sector] = header
    disk[2 * sector:2 * sector + len(entries)] = entries
    return bytes(disk) + partition + bytes(64 * 1024)

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)

def check(condition, message, failures):
    print(("PASS " if condition else "FAIL ") + message)
    if not condition: failures.append(message)

def extraction(tool, work, failures):
    partition = os.urandom(6 * 1024 * 1024 + 4096)  # crosses the 4 MiB header buffer
    for first_lba in (40, 4096, 12000):
        disk = gpt_image(partition, first_lba=first_lba)
        raw = os.path.join(work, f"disk-{first_lba}.img"); open(raw, "wb").write(disk)
        half = len(disk) // 2
        packed = os.path.join(work, f"disk-{first_lba}.img.bz2")
        open(packed, "wb").write(bz2.compress(disk[:half]) + bz2.compress(disk[half:]))  # pbzip2-style
        for source in (raw, packed):
            out = run([tool, "extract", source, "rootfs-A", "-"])
            check(out.returncode == 0 and hashlib.sha256(out.stdout).digest() == hashlib.sha256(partition).digest(),
                  f"extract rootfs-A at LBA {first_lba} from {os.path.basename(source)}", failures)
    missing = run([tool, "extract", raw, "rootfs-C", "-"])
    check(missing.returncode != 0 and b"not found" in missing.stderr, "missing partition is reported", failures)
    corrupt = bytearray(gpt_image(partition)); corrupt[2 * 512 + 60] ^= 0xff
    bad = os.path.join(work, "corrupt.img"); open(bad, "wb").write(corrupt)
    out = run([tool, "extract", bad, "rootfs-A", "-"])
    check(out.returncode != 0 and b"checksum" in out.stderr, "corrupt GPT entry table is rejected", failures)
    truncated = os.path.join(work, "truncated.img"); open(truncated, "wb").write(gpt_image(partition)[:4096 * 512 + 1024])
    out = run([tool, "extract", truncated, "rootfs-A", "-"])
    check(out.returncode != 0, "image ending inside the partition is rejected", failures)

def stream_import(fakefsify, work, failures):
    buffer = io.BytesIO()
    cap = bytes.fromhex("01000002" "00100000" "00000000" "00000000" "00000000")  # VFS_CAP_REVISION_2, CAP_NET_RAW
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as archive:
        def add(name, kind=tarfile.REGTYPE, data=b"", mode=0o644, uid=0, gid=0, link="", pax=None):
            info = tarfile.TarInfo(name); info.type = kind; info.mode = mode; info.uid = uid; info.gid = gid
            info.linkname = link; info.size = len(data) if kind == tarfile.REGTYPE else 0
            if pax: info.pax_headers = pax
            archive.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
        add("usr", tarfile.DIRTYPE, mode=0o755)
        add("usr/bin", tarfile.DIRTYPE, mode=0o755)
        add("usr/bin/tool", data=b"\x7fELF-not-run", mode=0o4755)
        add("usr/bin/alias", tarfile.LNKTYPE, link="usr/bin/tool", mode=0o4755)
        add("usr/bin/ping", data=b"ping", mode=0o755,
            pax={"SCHILY.xattr.security.capability": cap.decode("latin-1")})
        add("bin", tarfile.SYMTYPE, link="usr/bin", mode=0o777)
        add("home", tarfile.DIRTYPE, mode=0o755)
        add("home/deck", tarfile.DIRTYPE, mode=0o700, uid=1000, gid=1000)
    data = buffer.getvalue()
    root = os.path.join(work, "root")
    out = run([fakefsify, "-", root], input=data)
    check(out.returncode == 0, "fakefsify imports a PAX stream from standard input", failures)
    if out.returncode == 0:
        db = sqlite3.connect(os.path.join(root, "meta.db"))
        stats = {}
        for path, blob in db.execute("select path, stat from paths join stats on paths.inode = stats.inode"):
            stats[(path.decode() if isinstance(path, bytes) else path).lstrip("/")] = struct.unpack_from("<IIII", blob)
        def mode(p): return stats.get(p, (0, 0, 0, 0))
        check(mode("usr/bin/tool")[0] == 0o104755, "setuid regular file mode kept", failures)
        check(mode("home/deck")[0:3] == (0o40700, 1000, 1000), "directory owner and mode kept", failures)
        check(mode("bin")[0] & 0o170000 == 0o120000, "symbolic link recorded", failures)
        inode = {(p.decode() if isinstance(p, bytes) else p).lstrip("/"): i for p, i in db.execute("select path, inode from paths")}
        key = inode.get
        check(key("usr/bin/alias") is not None and key("usr/bin/alias") == key("usr/bin/tool"), "hard link shares the inode", failures)
        check(os.readlink(os.path.join(root, "data/bin")) if os.path.islink(os.path.join(root, "data/bin")) else open(os.path.join(root, "data/bin"), "rb").read() == b"usr/bin",
              "symbolic link target stored", failures)
        attrs = run(["xattr", "-l", os.path.join(root, "data/usr/bin/ping")])
        check(attrs.returncode == 0 and len(attrs.stdout) > 0, "file capability stored for the guest", failures)
    cut = run([fakefsify, "-", os.path.join(work, "cut")], input=data[:len(data) // 2 + 100])
    check(cut.returncode != 0, "truncated stream fails the import", failures)

def full_image(tool, image, failures):
    out = subprocess.Popen([tool, "export", image, "-"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    entries = files = caps = 0; total = 0; names = set()
    with tarfile.open(fileobj=out.stdout, mode="r|") as archive:
        for member in archive:
            entries += 1; names.add(member.name)
            if member.isfile():
                files += 1; total += member.size
                stream = archive.extractfile(member)
                while stream.read(1 << 20): pass
            caps += "SCHILY.xattr.security.capability" in member.pax_headers
    check(out.wait() == 0, "Valve rootfs-A exports without errors", failures)
    check(entries == 226048 and total == 8864478375, f"export size matches the 2026-11-08 measurement ({entries} entries, {total} bytes)", failures)
    check(caps == 10, f"file capabilities exported ({caps})", failures)
    check(not names & {"boot", "efi", "esp", "persist", "usr/lib/modules", "usr/lib/firmware", "usr/lib/debug"}, "kernel, firmware and boot data excluded", failures)
    check({"usr/bin/bash", "usr/bin/startplasma-x11", "usr/bin/kwin_x11", "usr/bin/plasmashell", "usr/bin/Xvfb"} <= names, "terminal and Plasma X11 binaries present", failures)

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tool", required=True); parser.add_argument("--fakefsify", required=True); parser.add_argument("--image")
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory() as work:
        extraction(args.tool, work, failures)
        stream_import(args.fakefsify, work, failures)
    if args.image: full_image(args.tool, args.image, failures)
    print(json.dumps({"result": "PASS" if not failures else "FAIL", "failures": failures}))
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
