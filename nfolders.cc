// NickelFolders -- rungs 1 and 2.
//
// Rung 1's whole job was answering whether an injected mod can hand an
// arbitrary ContentID to Nickel's stock reader and have the book open the
// way it does when you tap it in the library -- answered, see NOTES.md and
// README.md. Rung 2's job is a screen of ours on Nickel's own window stack
// (nfbrowser.cc) -- also drawing nothing on purpose, same as rung 1 opened
// no UI of its own, because the point of this rung is proving the screen
// itself works before anything is put on it.
//
// The libnickel call sequences live in nfnickel.cc (book-opening) and
// nfbrowser.cc (the screen), along with the inotify watch machinery. This
// file is left with the two trigger protocols and the NickelHook glue.
//
// Drive it from a shell, over ssh, with Nickel up:
//
//     echo 'file:///mnt/onboard/books/it/Some Book.epub' > /tmp/nfolders-open
//     touch /tmp/nfolders-show
//     logread | grep -i nickelfolders
//
// The open-trigger file is up to three lines: the ContentID, then getById's
// dbName (blank defers to the device's own correct value -- see nf_db_name in
// nfnickel.cc -- rather than always meaning "internal storage"), then how far
// to go, 1 to 4. Stopping short is how a crash gets localised to a single
// libnickel call; see nf_open_book_staged in nfnickel.cc. The show-trigger
// file's content is not read at all -- its EXISTENCE is the whole signal,
// matching the NickelMenu action Task 7's brief adds
// (`cmd_spawn :quiet:/bin/touch /tmp/nfolders-show`).

#include "nfbrowser.h"
#include "nfnickel.h"

#include <QString>
#include <QStringList>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <NickelHook.h>

#define NF_TRIGGER      "/tmp/nfolders-open"
#define NF_TRIGGER_SHOW "/tmp/nfolders-show"

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

// The show-trigger has no content protocol to parse -- its existence is the
// whole signal (see this file's opening comment) -- so this is a thin
// adapter from "the file appeared" to nf_browser_show()'s own signature.
// Runs on the GUI thread for the same reason nf_on_trigger does: nf_init
// (below) calls nf_watch_init from the GUI thread, and every callback it
// registers is invoked from the QSocketNotifier's activated() signal on
// that same thread -- no cross-thread hop to get wrong.
static void nf_on_trigger_show() {
    unlink(NF_TRIGGER_SHOW);
    nf_browser_show();
}

static int nf_init() {
    // Every NFNickelDlsym entry is optional (nfnickel.cc), so a miss here is
    // a real, reachable outcome now -- not hypothetical -- and this is the
    // only place it gets logged loudly. nf_open_book_staged and
    // nf_browser_show both still refuse to run rather than call through a
    // null pointer either way; the two checks are independent
    // (nf_nickel_resolve/nf_browser_resolve) so a firmware that breaks one
    // feature's symbols does not silently disable the other's too.
    if (!nf_nickel_resolve())
        nh_log("init: a libnickel symbol did not resolve; book-opening is inert until this is fixed");
    if (!nf_browser_resolve())
        nh_log("init: a libnickel symbol did not resolve; the browser screen is inert until this is fixed");

    // Both trigger files live directly in /tmp, so the second nf_watch_init
    // call below reuses the first's inotify fd/notifier rather than creating
    // a second one -- see nf_watch_init's own comment (nfnickel.h) for why
    // that is not a real limitation here. Non-fatal on failure, for both:
    // failing init here would trip NickelHook's shared failsafe and could
    // take other mods' installs down with it (CLAUDE.md), which is a wildly
    // disproportionate response to either trigger not being wired up.
    //
    // A non-zero return does not mean the watch wasn't built -- it may have
    // been, but is already known dead (nf_watch_init logged specifically why,
    // above this) -- either way the corresponding trigger will not work, so
    // this must not be followed by that trigger's "ready" line below.
    bool openReady = nf_watch_init(NF_TRIGGER, &nf_on_trigger) == 0;
    if (!openReady)
        nh_log("init: open-trigger watch is not usable; book-opening is inert (see 'watch:' lines above for why)");

    bool showReady = nf_watch_init(NF_TRIGGER_SHOW, &nf_on_trigger_show) == 0;
    if (!showReady)
        nh_log("init: show-trigger watch is not usable; the browser screen is inert (see 'watch:' lines above for why)");

    if (openReady)
        nh_log("init: ready, echo a ContentID into %s", NF_TRIGGER);
    if (showReady)
        nh_log("init: ready, touch %s to show the browser screen", NF_TRIGGER_SHOW);
    return 0;
}

static struct nh_info NFInfo = (struct nh_info){
    .name           = "NickelFolders",
    .desc           = "SPIKE: opens a book in the stock reader by ContentID, and can push an empty screen onto the window stack.",
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
