#!/usr/bin/env python3
"""Drive the Kobo test device over ssh.

Exists because this dev host has NO sshpass, NO expect and NO paramiko, so the
password has to be typed into a pty by hand. That is the whole trick, and it is
annoying enough to rediscover that it lives here rather than being retyped.

Connection details come from `device.local` (gitignored -- see DEVICE.local.md),
or from KOBO_HOST / KOBO_PASS / KOBO_PANEL in the environment.

    python3 tools/kobo.py ssh 'uptime; pidof nickel'
    python3 tools/kobo.py push libnfolders.so /usr/local/Kobo/imageformats/libnfolders.so
    python3 tools/kobo.py pull /usr/local/Kobo/libnickel.so.1.0.0 libnickel.so.1.0.0
    python3 tools/kobo.py shot screen.png
    python3 tools/kobo.py open 'file:///mnt/onboard/books/Some Book.epub' --stage 2
    python3 tools/kobo.py reboot
"""

import os
import pty
import select
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SSH_OPTS = [
    # Without these the pty hangs on a prompt instead of asking for a password.
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "-o", "PreferredAuthentications=password",
    "-o", "PubkeyAuthentication=no",
    "-o", "ConnectTimeout=10",
]


def config():
    cfg = {"host": "", "pass": "", "panel": ""}
    path = os.path.join(ROOT, "device.local")
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k.strip() in cfg:
                cfg[k.strip()] = v.strip()
    cfg["host"] = os.environ.get("KOBO_HOST", cfg["host"])
    cfg["pass"] = os.environ.get("KOBO_PASS", cfg["pass"])
    cfg["panel"] = os.environ.get("KOBO_PANEL", cfg["panel"])
    if not cfg["host"] or not cfg["pass"]:
        sys.exit("no device configured: create device.local (see DEVICE.local.md) "
                 "or set KOBO_HOST and KOBO_PASS")
    return cfg


def run_pty(argv, password, timeout):
    """Run argv under a pty, answering the first password prompt."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(argv[0], argv)

    out = b""
    sent = False
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 1.0)
        if fd in r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
            if not sent and b"assword" in out:
                os.write(fd, password.encode() + b"\n")
                sent = True
        else:
            try:
                wpid, _ = os.waitpid(pid, os.WNOHANG)
                if wpid:
                    break
            except ChildProcessError:
                break
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass

    text = out.decode("utf-8", "replace")
    # Drop everything up to and including the password prompt, so callers get
    # the command's own output and not ssh's banner.
    i = text.find("assword:")
    if i != -1:
        text = text[i + len("assword:"):]
    return text.lstrip("\r\n")


def do_ssh(cmd, timeout=300):
    c = config()
    return run_pty(["ssh"] + SSH_OPTS + ["root@" + c["host"], cmd], c["pass"], timeout)


def do_scp(src, dst, timeout=900):
    c = config()
    return run_pty(["scp", "-O"] + SSH_OPTS + [src, dst], c["pass"], timeout)


def cmd_shot(out):
    """Grab the framebuffer.

    Geometry is read off sysfs rather than hardcoded, but note that
    virtual_size is PADDED (1280x1792 on a Libra 2) while the visible panel is
    smaller (1264x1680) -- so the raw grab has garbage at the right and bottom
    edges unless it is cropped. `panel=WxH` in device.local supplies the crop.
    """
    from PIL import Image

    geo = do_ssh("cat /sys/class/graphics/fb0/virtual_size "
                 "/sys/class/graphics/fb0/bits_per_pixel "
                 "/sys/class/graphics/fb0/stride")
    nums = [l.strip() for l in geo.splitlines() if l.strip()]
    vw, vh = (int(x) for x in nums[0].split(","))
    bpp, stride = int(nums[1]), int(nums[2])
    if bpp != 32:
        sys.exit("only 32bpp framebuffers are handled; this one is %dbpp" % bpp)

    print("fb %dx%d %dbpp stride %d" % (vw, vh, bpp, stride))
    do_ssh("dd if=/dev/fb0 of=/tmp/fb.raw bs=%d count=%d 2>&1 | tail -1" % (stride, vh))
    raw = os.path.join(ROOT, ".fb.raw")
    do_scp("root@%s:/tmp/fb.raw" % config()["host"], raw)
    do_ssh("rm -f /tmp/fb.raw")

    data = open(raw, "rb").read()
    rows = [data[y * stride:y * stride + vw * 4] for y in range(vh)]
    img = Image.frombytes("RGBA", (vw, vh), b"".join(rows))
    b, g, r, _ = img.split()          # the panel is BGRA, not RGBA
    img = Image.merge("RGB", (r, g, b))

    panel = config()["panel"]
    if panel:
        pw, ph = (int(x) for x in panel.lower().split("x"))
        img = img.crop((0, 0, pw, ph))
    img.save(out)
    os.unlink(raw)
    print("wrote %s %s" % (out, img.size))


def cmd_open(content_id, db_name, stage):
    """Write the spike's trigger file and show what the mod logged."""
    payload = "%s\n%s\n%s\n" % (content_id, db_name, stage)
    # printf via a heredoc, so book titles with spaces and quotes survive.
    script = "cat > /tmp/nfolders-open <<'__NF__'\n%s__NF__\n" % payload
    do_ssh(script)
    print(do_ssh("sleep 5; logread | grep -i 'NickelFolders)' | grep -v NickelHook | tail -8; "
                 "echo '-- nickel pid:' $(pidof nickel); "
                 "echo '-- view:' $(timeout 15 qndb -m ndbCurrentView 2>&1 | tail -1)"))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    verb = sys.argv[1]

    if verb == "ssh":
        print(do_ssh(sys.argv[2]))
    elif verb == "push":
        print(do_scp(sys.argv[2], "root@%s:%s" % (config()["host"], sys.argv[3])))
    elif verb == "pull":
        print(do_scp("root@%s:%s" % (config()["host"], sys.argv[2]), sys.argv[3]))
    elif verb == "shot":
        cmd_shot(sys.argv[2] if len(sys.argv) > 2 else "screen.png")
    elif verb == "open":
        args = sys.argv[2:]
        stage, db = "4", ""
        if "--stage" in args:
            i = args.index("--stage")
            stage = args[i + 1]
            del args[i:i + 2]
        if "--db" in args:
            i = args.index("--db")
            db = args[i + 1]
            del args[i:i + 2]
        cmd_open(args[0], db, stage)
    elif verb == "reboot":
        # Backgrounded so ssh gets its reply out before the link drops.
        print(do_ssh("sync; (sleep 1; reboot) >/dev/null 2>&1 & echo reboot-issued", timeout=60))
    elif verb == "wait":
        # Polls until Nickel is up, which is what matters -- ssh answers before
        # Nickel has finished starting, and a mod that loads late looks absent.
        print("waiting for nickel ...")
        for _ in range(120):
            if "UP" in do_ssh("pidof nickel >/dev/null && echo UP", timeout=25):
                print("nickel is up")
                return
            time.sleep(5)
        sys.exit("timed out waiting for nickel")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
