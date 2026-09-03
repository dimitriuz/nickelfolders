// NickelFolders -- SPIKE.
//
// This file is a probe, not a product. Its entire job is to answer the one
// question the rest of the project depends on: can an injected mod hand an
// arbitrary ContentID to Nickel's stock reader and have the book open the way
// it does when you tap it in the library?
//
// It answers that by taking the same code path the library's "Read" button
// takes -- VolumeManager::getById to look the book up, then a
// ReadBookActionProxy over that Volume, then onSelected() -- so that all of
// Nickel's own bookkeeping (reading session, bookmark restore, analytics,
// download-if-needed) happens rather than being reimplemented.
//
// Drive it from a shell, over ssh, with Nickel up:
//
//     echo 'file:///mnt/onboard/books/it/Some Book.epub' > /tmp/nfolders-open
//     logread | grep -i nickelfolders
//
// The trigger file is up to three lines: the ContentID, then getById's dbName
// (blank is the default and the likely-correct value -- it is a runtime
// parameter because it is one of the things this spike exists to pin down),
// then how far to go, 1 to 4. Stopping short is how a crash gets localised to
// a single libnickel call; see nf_open_book.
//
// Nothing here draws anything. The browser is the next step and only makes
// sense once this has been seen to work.

#include <QByteArray>
#include <QStringList>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QString>

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <NickelHook.h>

#define NF_TRIGGER "/tmp/nfolders-open"

// Nickel's classes stay opaque. Declaring a real class for one would let a
// libnickel layout change turn into a silent miscompile, where an opaque
// pointer plus an explicitly-written call signature keeps every assumption
// about layout in one place and written down.
typedef void Volume;
typedef void ReadBookActionProxy;

// getById has NEITHER of the two implicit arguments it looks like it has.
//
// It returns a Volume BY VALUE, so the hidden return buffer is argument zero.
// And it is a STATIC member function, so there is no `this` at all -- the
// Itanium mangling is identical for static and non-static members, so the
// symbol name cannot tell you. Passing VolumeManager::sharedInstance() here as
// a `this` is what crashed Nickel on the first run: argument one is really the
// id, and Nickel read a QString out of the singleton pointer.
//
// NOTES.md has the disassembly that settles both halves, and how to re-check
// it on a new firmware.
static Volume *(*VolumeManager__getById)(void *ret, QString const *id, QString const *dbName);
static bool    (*Volume__isValid)(Volume const *_this);
static void    (*Volume__dtor)(Volume *_this);
static void    (*ReadBookActionProxy__ctor)(ReadBookActionProxy *_this, QObject *parent, Volume const *v);
static void    (*ReadBookActionProxy__onSelected)(ReadBookActionProxy *_this);

// The poller thread never touches Qt beyond postEvent (which is documented
// thread-safe); it hands the ContentID over through this buffer instead.
static pthread_mutex_t nf_lock = PTHREAD_MUTEX_INITIALIZER;
static char            nf_pending[2048];
static int             nf_event_type;
static QObject        *nf_trigger;

// nf_open_book runs on the GUI thread. Everything it calls is Nickel UI code,
// which is why the poller does not call it directly.
//
// `stage` stops after the 1st, 2nd or 3rd libnickel call. A wrong guess about
// any of these signatures takes Nickel down with it, and the first run proved
// that is not hypothetical -- so the probe is built to be advanced one call at
// a time, because a crash at a known stage names the culprit and a crash after
// all three does not.
static void nf_open_book(QString const& contentId, QString const& dbName, int stage) {
    nh_log("open: stage=%d contentId='%s' dbName='%s'", stage, qPrintable(contentId), qPrintable(dbName));

    // A Volume is 8 bytes on 4.38.23684 -- a vptr plus a refcounted pointer to
    // a 408-byte shared block, which is why copying one is cheap. The buffer is
    // deliberately much larger: getById constructs into it and cannot be told
    // how big it is, so the headroom is what absorbs a firmware that grows the
    // object. NOTES.md says how to re-measure it.
    unsigned char volbuf[128] __attribute__((aligned(8)));
    memset(volbuf, 0, sizeof volbuf);

    Volume *v = VolumeManager__getById(volbuf, &contentId, &dbName);
    if (!v) {
        nh_log("open: getById returned null, giving up");
        return;
    }

    nh_log("open: getById returned %p", v);
    if (stage < 2) {
        nh_log("open: stopping after getById as asked");
        Volume__dtor(v);
        return;
    }

    // getById answers for an unknown ContentID with a default-constructed
    // Volume rather than an error, so isValid is the only thing that
    // distinguishes "found it" from "no such book".
    bool valid = Volume__isValid(v);
    nh_log("open: isValid=%s", valid ? "true" : "false");
    if (!valid) {
        nh_log("open: no book in the library has that ContentID");
        Volume__dtor(v);
        return;
    }
    if (stage < 3) {
        nh_log("open: stopping before the proxy as asked");
        Volume__dtor(v);
        return;
    }

    // 52 bytes on this firmware, read out of the `operator new` call inside
    // ActionProxyMixin::readBookProxy -- i.e. Nickel's own allocation for this
    // exact object. Over-allocated for the same reason as volbuf above.
    ReadBookActionProxy *proxy = static_cast<ReadBookActionProxy*>(::operator new(512));
    memset(proxy, 0, 512);

    // The parent is passed straight through to the QObject base, so nf_trigger
    // owns the proxy from here on. SPIKE ONLY: that leaks 52 bytes per open,
    // which is the deliberate trade -- onSelected() may kick off an async
    // worker that still refers to the proxy, and leaking a handful of bytes
    // during a probe beats freeing an object out from under Nickel.
    ReadBookActionProxy__ctor(proxy, nf_trigger, v);
    nh_log("open: proxy constructed at %p", proxy);

    if (stage < 4) {
        nh_log("open: stopping before onSelected as asked");
        Volume__dtor(v);
        return;
    }

    nh_log("open: calling onSelected()");
    ReadBookActionProxy__onSelected(proxy);
    nh_log("open: onSelected() returned");

    // Destroyed after onSelected, not before: the proxy holds its own copy
    // (the shared block is refcounted), but ordering it this way means the
    // block cannot go away mid-call even if that ever stops being true.
    Volume__dtor(v);
}

class NFTrigger : public QObject {
public:
    // No Q_OBJECT and no moc: this object has no signals or slots of its own,
    // and event() is virtual already.
    bool event(QEvent *e) {
        if (e->type() != nf_event_type)
            return QObject::event(e);

        char buf[sizeof nf_pending];
        pthread_mutex_lock(&nf_lock);
        memcpy(buf, nf_pending, sizeof buf);
        nf_pending[0] = '\0';
        pthread_mutex_unlock(&nf_lock);

        // Line 1 is the ContentID, line 2 is dbName (blank allowed), line 3 is
        // how far to go: 1 getById, 2 + isValid, 3 + construct the proxy,
        // 4 + onSelected. Default 4, so a one-line trigger still means "open
        // the book"; the lower rungs exist to localise a crash.
        QStringList lines = QString::fromUtf8(buf).split(QLatin1Char('\n'));

        QString contentId = lines.value(0).trimmed();
        QString dbName    = lines.value(1).trimmed();
        int     stage     = 4;
        QString stageStr  = lines.value(2).trimmed();
        if (!stageStr.isEmpty()) {
            bool ok = false;
            int n = stageStr.toInt(&ok);
            if (ok && n >= 1 && n <= 4)
                stage = n;
            else
                nh_log("trigger: bad stage '%s', using 4", qPrintable(stageStr));
        }

        if (contentId.isEmpty()) {
            nh_log("trigger: empty, ignoring");
            return true;
        }

        // Typing the scheme every time is a nuisance, and a bare path is
        // unambiguous here.
        if (!contentId.contains(QStringLiteral("://")))
            contentId = QStringLiteral("file://") + contentId;

        nf_open_book(contentId, dbName, stage);
        return true;
    }
};

// nf_poll_thread deliberately watches a path on /tmp: it is tmpfs, so this
// never holds a handle on /mnt/onboard, which would risk trouble the moment
// the device is plugged in over USB.
static void *nf_poll_thread(void *) {
    for (;;) {
        struct timespec ts = { 0, 500 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        int fd = open(NF_TRIGGER, O_RDONLY);
        if (fd < 0)
            continue;

        char buf[sizeof nf_pending];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        unlink(NF_TRIGGER);
        if (n <= 0)
            continue;
        buf[n] = '\0';

        pthread_mutex_lock(&nf_lock);
        memcpy(nf_pending, buf, sizeof nf_pending);
        pthread_mutex_unlock(&nf_lock);

        // Nickel's UI must not be touched from here; posting an event hands
        // the work to the thread nf_trigger lives on.
        if (QCoreApplication::instance())
            QCoreApplication::postEvent(nf_trigger, new QEvent(static_cast<QEvent::Type>(nf_event_type)));
        else
            nh_log("trigger: no QCoreApplication yet, dropping");
    }
    return NULL;
}

static int nf_init() {
    nf_event_type = QEvent::registerEventType();
    nf_trigger    = new NFTrigger();

    pthread_t t;
    if (pthread_create(&t, NULL, nf_poll_thread, NULL) != 0) {
        // Non-fatal on purpose. Failing init here would trip NickelHook's
        // failsafe and take other mods' installs down with it, which is a
        // wildly disproportionate response to a spike that cannot be
        // triggered.
        nh_log("init: could not start the trigger thread; mod is inert");
        return 0;
    }
    pthread_detach(t);

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

static struct nh_dlsym NFDlsym[] = {
    {.name = "_ZN13VolumeManager7getByIdERK7QStringS2_",      .out = nh_symoutptr(VolumeManager__getById),          .desc = "VolumeManager::getById"},
    {.name = "_ZNK6Volume7isValidEv",                         .out = nh_symoutptr(Volume__isValid),                 .desc = "Volume::isValid"},
    {.name = "_ZN6VolumeD1Ev",                                .out = nh_symoutptr(Volume__dtor),                    .desc = "Volume::~Volume"},
    {.name = "_ZN19ReadBookActionProxyC1EP7QObjectRK6Volume", .out = nh_symoutptr(ReadBookActionProxy__ctor),       .desc = "ReadBookActionProxy::ReadBookActionProxy"},
    {.name = "_ZN19ReadBookActionProxy10onSelectedEv",        .out = nh_symoutptr(ReadBookActionProxy__onSelected), .desc = "ReadBookActionProxy::onSelected"},
    {0},
};

NickelHook(
    .init  = &nf_init,
    .info  = &NFInfo,
    .hook  = NULL,
    .dlsym = NFDlsym,
)
