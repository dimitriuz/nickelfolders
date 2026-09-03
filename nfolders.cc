// NickelFolders -- SPIKE, rung 1.
//
// This file is still a probe, not a product: its whole job remains answering
// whether an injected mod can hand an arbitrary ContentID to Nickel's stock
// reader and have the book open the way it does when you tap it in the
// library. That question is answered -- see NOTES.md and README.md -- so this
// rung's job is different: it pays off the three things the spike knowingly
// got wrong (a hardcoded dbName, a leaked proxy, a 500 ms poll thread) before
// five more libnickel calls get built on top of it. Nothing about WHAT the
// mod does changes here.
//
// The libnickel call sequence itself -- VolumeManager::getById, then a
// ReadBookActionProxy over the Volume, then onSelected() -- now lives in
// nfnickel.cc, along with the inotify watch that replaces the poll thread.
// This file is left with the trigger protocol and the NickelHook glue.
//
// Drive it from a shell, over ssh, with Nickel up:
//
//     echo 'file:///mnt/onboard/books/it/Some Book.epub' > /tmp/nfolders-open
//     logread | grep -i nickelfolders
//
// The trigger file is up to three lines: the ContentID, then getById's dbName
// (blank now defers to the device's own correct value -- see nf_db_name in
// nfnickel.cc -- rather than always meaning "internal storage"), then how far
// to go, 1 to 4. Stopping short is how a crash gets localised to a single
// libnickel call; see nf_open_book_staged in nfnickel.cc.
//
// Nothing here draws anything. The browser is the next step and only makes
// sense once this has been seen to work.

#include "nfnickel.h"

#include <QString>
#include <QStringList>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <NickelHook.h>

#define NF_TRIGGER "/tmp/nfolders-open"

// nf_on_trigger runs on the GUI thread already: nf_watch_init's callback is
// invoked from the QSocketNotifier's activated() signal, which the Qt event
// loop delivers on whatever thread set the watch up, and nf_init (below) runs
// on the GUI thread. That is what makes it safe to call straight through to
// nf_open_book_staged, which is Nickel UI code -- there is no cross-thread hop
// left to get wrong, unlike the spike's postEvent to a poll thread.
static void nf_on_trigger() {
    // The watch fires on IN_CLOSE_WRITE/IN_MOVED_TO for this exact name, so by
    // the time this runs the file exists and is fully written.
    int fd = open(NF_TRIGGER, O_RDONLY);
    if (fd < 0) {
        nh_log("trigger: %s fired but could not be opened: %s", NF_TRIGGER, strerror(errno));
        return;
    }

    char buf[2048];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    unlink(NF_TRIGGER);
    if (n <= 0)
        return;
    buf[n] = '\0';

    // Line 1 is the ContentID, line 2 is dbName, line 3 is how far to go:
    // 1 getById, 2 + isValid, 3 + construct the proxy, 4 + onSelected.
    // Default 4, so a one-line trigger still means "open the book"; the lower
    // rungs exist to localise a crash (CLAUDE.md, "Method: adding a new
    // libnickel call").
    QStringList lines = QString::fromUtf8(buf).split(QLatin1Char('\n'));

    QString contentId = lines.value(0).trimmed();
    if (contentId.isEmpty()) {
        nh_log("trigger: empty, ignoring");
        return;
    }
    // Typing the scheme every time is a nuisance, and a bare path is
    // unambiguous here.
    if (!contentId.contains(QStringLiteral("://")))
        contentId = QStringLiteral("file://") + contentId;

    // Blank defers to nf_db_name(), the device's own correct value -- typing
    // one here is for deliberately OVERRIDING it, e.g. to rehearse the
    // SD-card path this particular device cannot physically exercise
    // (NOTES.md #6). This is the same default nf_open_book(QString const&)
    // uses; the trigger protocol is not a second, worse-informed code path.
    QString dbName = lines.value(1).trimmed();
    if (dbName.isEmpty()) {
        QString const *d = nf_db_name();
        if (d)
            dbName = *d;
    }

    int     stage    = 4;
    QString stageStr = lines.value(2).trimmed();
    if (!stageStr.isEmpty()) {
        bool ok = false;
        int  s  = stageStr.toInt(&ok);
        if (ok && s >= 1 && s <= 4)
            stage = s;
        else
            nh_log("trigger: bad stage '%s', using 4", qPrintable(stageStr));
    }

    nf_open_book_staged(contentId, dbName, stage);
}

static int nf_init() {
    // Every NFNickelDlsym entry is optional (nfnickel.cc), so a miss here is
    // a real, reachable outcome now -- not hypothetical -- and this is the
    // only place it gets logged loudly. nf_open_book_staged still refuses to
    // run rather than call through a null pointer either way.
    if (!nf_nickel_resolve())
        nh_log("init: a libnickel symbol did not resolve; book-opening is inert until this is fixed");

    if (nf_watch_init(NF_TRIGGER, &nf_on_trigger) != 0) {
        // Non-fatal on purpose. Failing init here would trip NickelHook's
        // shared failsafe and could take other mods' installs down with it
        // (CLAUDE.md), which is a wildly disproportionate response to a mod
        // that, at this rung, still has no UI and cannot be triggered any
        // other way.
        nh_log("init: could not set up the trigger watch; mod is inert");
        return 0;
    }

    nh_log("init: ready, echo a ContentID into %s", NF_TRIGGER);
    return 0;
}

static struct nh_info NFInfo = (struct nh_info){
    .name           = "NickelFolders",
    .desc           = "SPIKE: opens a book in the stock reader by ContentID.",
    .uninstall_flag = "/mnt/onboard/nfolders_uninstall",
    // Spelled out although it is unused: GCC 4.9's C++ frontend rejects a
    // designated initializer that SKIPS a field ("non-trivial designated
    // initializers not supported"), so every field up to the last one set
    // has to appear, in order.
    .uninstall_xflag = NULL,
    .failsafe_delay  = 3,
};

NickelHook(
    .init  = &nf_init,
    .info  = &NFInfo,
    .hook  = NULL,
    .dlsym = NFNickelDlsym,
)
