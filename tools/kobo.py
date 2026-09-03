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
import signal
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
    """Run argv under a pty, answering the first password prompt.

    Returns (text, exit_code). exit_code is the child's real exit status
    (ssh's own, which for the `ssh` verb IS the remote command's status) --
    or None if `timeout` elapsed before the child exited, which callers must
    treat as a failure, not as "probably fine." Three device-session bugs
    all traced back to this function returning only text and every caller
    assuming that meant success:

    - `push` reported success for an scp that actually printed
      "Read-only file system" and exited non-zero.
    - `reboot` reported success for an ssh that hung past its own
      ConnectTimeout ("Connection timed out") and never ran anything.

    Both looked identical to a real success until the exit code was
    actually read, so it now is.
    """
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(argv[0], argv)

    out = b""
    sent = False
    status = None
    deadline = time.time() + timeout
    while status is None and time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 1.0)
        if fd in r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                chunk = b""
            if chunk:
                out += chunk
                if not sent and b"assword" in out:
                    os.write(fd, password.encode() + b"\n")
                    sent = True
                continue
            # EOF on the pty: the child closed its end, which normally means
            # it is exiting. Reap it for the REAL exit status rather than
            # assuming the empty read meant success.
            try:
                _, status = os.waitpid(pid, 0)
            except ChildProcessError:
                status = -1  # already reaped elsewhere; nothing left to learn
        else:
            try:
                wpid, wstatus = os.waitpid(pid, os.WNOHANG)
                if wpid:
                    status = wstatus
            except ChildProcessError:
                status = -1

    if status is None:
        # The deadline was hit with the child still running. Observed for
        # `reboot`: the device had already dropped off wifi, ssh hung on the
        # dead TCP connection well past what its own ConnectTimeout should
        # have caught (the timeout applies to the initial handshake, not to
        # a connection that dies after being established), and the OLD code
        # returned whatever partial text it had collected with an implicit
        # "it must have worked" -- the device's uptime afterwards proved it
        # had not rebooted. Kill the still-running child and report a
        # timeout; never let a hang read as quiet success.
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except (OSError, ChildProcessError):
            pass
        exit_code = None
    elif os.WIFEXITED(status):
        exit_code = os.WEXITSTATUS(status)
    elif os.WIFSIGNALED(status):
        exit_code = -os.WTERMSIG(status)
    else:
        exit_code = -1

    text = out.decode("utf-8", "replace")
    # Drop everything up to and including the password prompt, so callers get
    # the command's own output and not ssh's banner.
    i = text.find("assword:")
    if i != -1:
        text = text[i + len("assword:"):]
    return text.lstrip("\r\n"), exit_code


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

    geo, _ = do_ssh("cat /sys/class/graphics/fb0/virtual_size "
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
    # A failed pull here is not silent the way push/reboot/wait's bugs were:
    # a short or missing .fb.raw makes the frombytes()/read() below raise
    # immediately, so no separate exit-code check is added -- the existing
    # failure is already loud.
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
    text, _ = do_ssh("sleep 5; logread | grep -i 'NickelFolders)' | grep -v NickelHook | tail -8; "
                     "echo '-- nickel pid:' $(pidof nickel); "
                     "echo '-- view:' $(timeout 15 qndb -m ndbCurrentView 2>&1 | tail -1)")
    print(text)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    verb = sys.argv[1]

    if verb == "ssh":
        # No exit-code enforcement here on purpose: this verb is a raw
        # remote-command runner (`pidof nickel` legitimately returns
        # non-zero when nickel isn't running), so making a non-zero remote
        # exit fatal would be a new behavior, not a bug fix, and would break
        # callers that run a command specifically to see it fail.
        text, _ = do_ssh(sys.argv[2])
        print(text)
    elif verb == "push":
        text, code = do_scp(sys.argv[2], "root@%s:%s" % (config()["host"], sys.argv[3]))
        print(text)
        if code != 0:
            # Observed: `scp: /usr/local/Kobo/imageformats/libnfolders.so:
            # Read-only file system`, printed to `text` above, and the OLD
            # code still exited 0 -- so the push "succeeded," the next
            # reboot ran whatever .so was already on the device, and
            # whatever was learned from testing it was about stale code.
            # scp's own exit status is what actually says whether the file
            # landed; trust it, not the absence of a caught exception.
            sys.exit("push failed (scp exit %s)" % code)
    elif verb == "pull":
        # Same failure shape as push (silent partial/failed transfer,
        # exit 0), fixed the same way -- pull just runs less often, so it
        # had not yet been caught misreporting on a device session.
        text, code = do_scp("root@%s:%s" % (config()["host"], sys.argv[2]), sys.argv[3])
        print(text)
        if code != 0:
            sys.exit("pull failed (scp exit %s)" % code)
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
        text, code = do_ssh(
            "sync; (sleep 1; reboot) >/dev/null 2>&1 & echo reboot-issued", timeout=60)
        print(text)
        if code != 0:
            # Observed: the device had already dropped off the network, ssh
            # printed "ssh: connect to host ... Connection timed out", and
            # the OLD code still exited 0 -- `reboot && wait` chained
            # straight past the failure, `wait` then polled a device that
            # was never asked to reboot, and the device's own `uptime`
            # afterwards proved it. Never report reboot-issued for an ssh
            # that did not actually issue it.
            sys.exit("reboot failed (ssh exit %s): %s" % (code, text.strip()))
    elif verb == "wait":
        # Polls until Nickel is up AND has just booted, not merely until
        # some process named nickel answers pidof. That second half is not
        # paranoia: caught on a real device session, `wait` matched the
        # OUTGOING Nickel while it was still on its way down for the very
        # reboot this call exists to wait out (a killed process is not gone
        # instantly), reported "nickel is up," and a `pidof nickel` a few
        # seconds later came back empty with the newest log line still from
        # the PREVIOUS boot -- which looks exactly like a boot loop and is
        # not one. A pid alone cannot tell a dying old Nickel from a fresh
        # one; /proc/uptime can, because the old process's uptime cannot be
        # small.
        #
        # UPTIME_THRESHOLD is generous against how long this device actually
        # takes to bring Nickel up (observed well under a minute) while
        # staying far below "has been running for any real length of time,"
        # so a genuinely stale Nickel from before the reboot can never
        # alias a fresh one by getting lucky on timing.
        UPTIME_THRESHOLD = 120
        print("waiting for nickel ...")
        for _ in range(120):
            text, code = do_ssh(
                "pidof nickel >/dev/null && cat /proc/uptime", timeout=25)
            if code == 0 and text.strip():
                uptime = float(text.split()[0])
                if uptime < UPTIME_THRESHOLD:
                    print("nickel is up (uptime %.1fs)" % uptime)
                    return
                # pidof found a process, but it is too old to be the one
                # from THIS boot -- keep polling rather than declaring
                # victory on the outgoing Nickel.
            time.sleep(5)
        sys.exit("timed out waiting for nickel")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
