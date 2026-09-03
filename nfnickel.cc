// NickelFolders -- libnickel call surface + trigger watch.
//
// This is where two of the three debts nfolders.cc (the spike) deliberately
// incurred get paid -- the hardcoded dbName and the 500 ms poll thread -- and
// where the third (the leaked ReadBookActionProxy) gets accurately documented
// instead: parenting it to an immortal owner does not free it, only defers it
// forever, so the footprint is unchanged from the spike. See the comment on
// nf_proxy_owner and nf_open_book_staged for why, and NOTES.md's open items
// for the archaeology freeing it would need. Nothing about what the mod DOES
// changes here -- see nf_open_book_staged, which is
// nf_open_book(QString,QString,int) from the spike, unchanged except for its
// return value.

#include "nfnickel.h"

#include <QAbstractEventDispatcher>
#include <QByteArray>
#include <QCoreApplication>
#include <QObject>
#include <QSocketNotifier>
#include <QString>

#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h> // NAME_MAX
#include <stdio.h>        // snprintf
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

// Nickel's classes stay opaque: typedef + an explicitly written call
// signature, never a real C++ class or a redeclared method. A real class
// would turn a libnickel layout change into a silent miscompile; this form
// keeps every layout assumption in one place and written down. NOTES.md has
// the disassembly that establishes each signature below.
typedef void Volume;
typedef void ReadBookActionProxy;
typedef void Device;

// getById has NEITHER of the two implicit arguments it looks like it has. It
// returns a Volume BY VALUE, so the hidden return buffer is argument zero.
// And it is a STATIC member function -- there is no `this` at all -- because
// the Itanium ABI mangles static and non-static members identically, so the
// symbol name cannot tell you. Reading argument one as `this` is what crashed
// Nickel on the spike's first device run: it is really the id, and Nickel
// read a QString out of the singleton pointer.
static Volume *(*VolumeManager__getById)(void *ret, QString const *id, QString const *dbName);
static bool    (*Volume__isValid)(Volume const *_this);
static void    (*Volume__dtor)(Volume *_this);
static void    (*ReadBookActionProxy__ctor)(ReadBookActionProxy *_this, QObject *parent, Volume const *v);
static void    (*ReadBookActionProxy__onSelected)(ReadBookActionProxy *_this);

// Device::getCurrentDevice is a tail-call to getCurrentDeviceMutable and
// returns a Device* in r0. Device::getDbName is three instructions --
// ldr/adds/bx -- returning a QString const& into the Device's own already-
// cached field: nothing allocated, nothing to free. NOTES.md #6 has both
// disassemblies and the calcDbName derivation that explains what the string
// means (empty for internal storage, non-empty on an SD card).
static Device        *(*Device__getCurrentDevice)(void);
static QString const *(*Device__getDbName)(Device const *_this);

// Every entry below is .optional = true, and that is not carelessness on
// getById of all things -- it is the shared-failsafe rule in CLAUDE.md taken
// seriously. NickelHook.c resolves this array BEFORE nf_init ever runs
// (nh.c:158-167), and a non-optional miss there is fatal at that pass --
// nf_nickel_resolve()'s gate below and nf_open_book_staged's refusal to run
// are both unreachable in exactly the scenario they exist for, because
// NickelHook already gave up before either could run. A fatal init trips
// NickelHook's failsafe, which is SHARED: this device also runs NickelMenu,
// NickelDBus, kfmon and KOReader, so a symbol renamed on a firmware bump
// could take their installs down along with this one. Marking every entry
// optional means a miss instead resolves to NULL, gets logged as "is
// optional so ignoring", and leaves nf_nickel_resolve() to catch it and make
// book-opening (and only book-opening) inert -- matching what NOTES.md
// already claims happens ("if a symbol stops resolving, NickelHook logs it
// and disarms rather than crashing").
struct nh_dlsym NFNickelDlsym[] = {
    {.name = "_ZN13VolumeManager7getByIdERK7QStringS2_",      .out = nh_symoutptr(VolumeManager__getById),          .desc = "VolumeManager::getById",                 .optional = true},
    {.name = "_ZNK6Volume7isValidEv",                         .out = nh_symoutptr(Volume__isValid),                 .desc = "Volume::isValid",                        .optional = true},
    {.name = "_ZN6VolumeD1Ev",                                .out = nh_symoutptr(Volume__dtor),                    .desc = "Volume::~Volume",                        .optional = true},
    {.name = "_ZN19ReadBookActionProxyC1EP7QObjectRK6Volume", .out = nh_symoutptr(ReadBookActionProxy__ctor),       .desc = "ReadBookActionProxy::ReadBookActionProxy", .optional = true},
    {.name = "_ZN19ReadBookActionProxy10onSelectedEv",        .out = nh_symoutptr(ReadBookActionProxy__onSelected), .desc = "ReadBookActionProxy::onSelected",        .optional = true},
    {.name = "_ZN6Device16getCurrentDeviceEv",                .out = nh_symoutptr(Device__getCurrentDevice),        .desc = "Device::getCurrentDevice",               .optional = true},
    {.name = "_ZNK6Device9getDbNameEv",                       .out = nh_symoutptr(Device__getDbName),               .desc = "Device::getDbName",                      .optional = true},
    {0},
};

bool nf_nickel_resolve(void) {
    return VolumeManager__getById && Volume__isValid && Volume__dtor &&
           ReadBookActionProxy__ctor && ReadBookActionProxy__onSelected &&
           Device__getCurrentDevice && Device__getDbName;
}

// dbName is a Repository cache-partition key. Device::calcDbName compares the
// device's own storage path against the literal /mnt/onboard/.kobo and
// returns empty ONLY on a match, deriving a name via QDir::cleanPath
// otherwise -- so the spike's hardcoded "" measured correct on this Libra 2
// for a REASON (it has no SD slot), and would silently find nothing on a
// model that does. Nickel itself never passes a constant: all 117 call sites
// of getById pass this in a register computed just before the call. NOTES.md
// #6 has the full derivation.
//
// Both callers (nf_open_book, and nf_on_trigger in nfolders.cc) reach this
// BEFORE nf_open_book_staged's nf_nickel_resolve() gate -- they need a
// dbName to pass INTO that call, so this cannot lean on that gate having
// already run. With every NFNickelDlsym entry .optional = true, a firmware
// that renames either symbol below leaves these two function pointers NULL,
// and calling through a NULL function pointer is `blx` to address 0 inside
// Nickel -- so this function guards itself rather than assuming a caller did.
QString const *nf_db_name(void) {
    if (!Device__getCurrentDevice || !Device__getDbName)
        return NULL;
    Device *dev = Device__getCurrentDevice();
    if (!dev)
        return NULL;
    return Device__getDbName(dev);
}

// The proxy's `parent` argument is passed straight through to its QObject
// base -- confirmed in the disassembly of readBookProxy, NOTES.md -- so any
// QObject of ours works as the parent, and Qt destroys a child no later than
// when its parent is destroyed. But `owner` here is NEVER destroyed -- it is
// a function-local static, constructed once on first use and never freed --
// so parenting every proxy to it does not free anything; it only defers every
// proxy to an object that outlives the process. THIS DOES NOT PAY THE LEAK
// DEBT. See the comment where the proxy is constructed, below, for what is
// and is not established about freeing it, and why this rung ships it
// unfreed on purpose rather than guessing.
static QObject *nf_proxy_owner() {
    static QObject *owner = new QObject();
    return owner;
}

// nf_open_book_staged runs on the GUI thread. Everything it calls is Nickel UI
// code, which is why nothing upstream of it (nf_on_trigger, in nfolders.cc)
// calls it from anywhere else.
//
// `stage` stops after the 1st, 2nd or 3rd libnickel call. A wrong guess about
// any of these signatures takes Nickel down with it -- the spike's first run
// proved that is not hypothetical -- so this stays built to advance one call
// at a time: a crash at a known stage names the culprit, a crash after all of
// them does not. CLAUDE.md, "Method: adding a new libnickel call", depends on
// this staying available for the next five rungs.
bool nf_open_book_staged(QString const& contentId, QString const& dbName, int stage) {
    nh_log("open: stage=%d contentId='%s' dbName='%s'", stage, qPrintable(contentId), qPrintable(dbName));

    if (!nf_nickel_resolve()) {
        nh_log("open: a required symbol never resolved, refusing to call through a null pointer");
        return false;
    }

    // A Volume is 8 bytes on 4.38.23684 -- a vptr plus a refcounted pointer to
    // a 408-byte shared block, which is why copying one is cheap. The buffer
    // is deliberately much larger: getById constructs into it and cannot be
    // told how big it is, so the headroom is what absorbs a firmware that
    // grows the object. NOTES.md says how to re-measure it.
    unsigned char volbuf[128] __attribute__((aligned(8)));
    memset(volbuf, 0, sizeof volbuf);

    Volume *v = VolumeManager__getById(volbuf, &contentId, &dbName);
    if (!v) {
        nh_log("open: getById returned null, giving up");
        return false;
    }

    nh_log("open: getById returned %p", v);
    if (stage < 2) {
        nh_log("open: stopping after getById as asked");
        Volume__dtor(v);
        return true;
    }

    // getById answers for an unknown ContentID with a default-constructed
    // Volume rather than an error, so isValid is the only thing that
    // distinguishes "found it" from "no such book".
    bool valid = Volume__isValid(v);
    nh_log("open: isValid=%s", valid ? "true" : "false");
    if (!valid) {
        nh_log("open: no book in the library has that ContentID");
        Volume__dtor(v);
        return false;
    }
    if (stage < 3) {
        nh_log("open: stopping before the proxy as asked");
        Volume__dtor(v);
        return true;
    }

    // sizeof(ReadBookActionProxy) == 52 on this firmware, read out of the
    // `operator new` call inside ActionProxyMixin::readBookProxy -- i.e.
    // Nickel's own allocation for this exact object. We ask our own operator
    // new for 512, not 52: over-allocated for the same reason as volbuf
    // above, and that headroom is what makes the real per-open cost 512
    // bytes, not the measured object size -- see below.
    ReadBookActionProxy *proxy = static_cast<ReadBookActionProxy*>(::operator new(512));
    memset(proxy, 0, 512);

    // NOT FREED, and parenting to nf_proxy_owner() (above) does not change
    // that: that owner is never destroyed, so Qt never destroys this child
    // either -- the proxy is deferred to an immortal object, not freed. The
    // per-open footprint is therefore UNCHANGED from the spike's deliberate
    // leak: 512 bytes/open (this call's own allocation request, not the
    // measured 52-byte object), ~10 kB per 20 opens, on the order of 300 kB a
    // month at twenty opens a day.
    //
    // This is a known, documented debt, not an oversight: freeing it needs
    // first establishing whether onSelected() (0x00c8bacc) frees or
    // self-deletes the proxy on some path. NOTES.md already records that it
    // allocates a 28-byte worker on one path -- exactly why the spike never
    // freed it -- and resolving onSelected's PLT stubs with tools/plt.sh to
    // settle that did not succeed. Shipping a speculative deleteLater() on an
    // object whose lifetime Nickel may already control is the same class of
    // guess that crashed Nickel once already (VolumeManager::getById's
    // history, this file's opening comment) -- not a trade worth making
    // blind. NOTES.md's open items records the archaeology this needs.
    ReadBookActionProxy__ctor(proxy, nf_proxy_owner(), v);
    nh_log("open: proxy constructed at %p", proxy);

    if (stage < 4) {
        nh_log("open: stopping before onSelected as asked");
        Volume__dtor(v);
        return true;
    }

    nh_log("open: calling onSelected()");
    ReadBookActionProxy__onSelected(proxy);
    nh_log("open: onSelected() returned");

    // Destroyed after onSelected, not before: the proxy holds its own copy
    // (the shared block is refcounted), but ordering it this way means the
    // block cannot go away mid-call even if that ever stops being true.
    Volume__dtor(v);
    return true;
}

bool nf_open_book(QString const& contentId) {
    // NULL only if Device::getCurrentDevice() itself failed -- see nf_db_name
    // -- in which case "" (this device's own internal-storage value, and the
    // spike's old hardcoded default) is the least surprising fallback rather
    // than refusing outright.
    QString const *db = nf_db_name();
    static QString const empty;
    return nf_open_book_staged(contentId, db ? *db : empty, 4);
}

// --- inotify watch --------------------------------------------------------
//
// The 500 ms poll thread was a probe mechanism. inotify through a
// QSocketNotifier is event-driven, runs on the GUI thread (one of the two
// safe windows for touching Nickel, per CLAUDE.md), and needs no second
// thread at all -- which also removes the cross-thread postEvent hop the
// poller needed to reach the GUI thread safely.
//
// The watch is on a /tmp path deliberately: /tmp is tmpfs, so this never
// holds a handle on /mnt/onboard, where one open during a USB session risks
// corruption. It also means the watch directory is recreated after every
// reboot, same as before.

static void (*nf_watch_cb)(void) = NULL;

// POD, NOT QByteArray, and this is load-bearing, not style: a file-scope
// QByteArray here previously segfaulted Nickel on every boot. NickelHook's
// __attribute__((constructor)) nh_init runs from THIS library's .init_array
// before this translation unit's own C++ dynamic initialiser
// (_GLOBAL__sub_I_nfnickel.cc) does -- confirmed with readelf, see the fix
// report -- and nh_init calls straight through nf_init -> nf_watch_init
// before that initialiser has run. A QByteArray assignment there dereferenced
// a `d` pointer that had never been constructed (still zeroed .bss), which is
// a load from address 0. POD types are always zero-initialised by .bss
// itself, with no constructor to race against, which is why NO file-scope
// object in this translation unit may have a non-trivial (dynamically
// initialised) constructor: this is the only kind of global whose
// initialisation order relative to nh_init is not guaranteed. Every other
// static above and below this line is a plain pointer for exactly this
// reason -- do not add another QString/QByteArray/QObject/etc. at file scope.
static char             nf_watch_name[NAME_MAX + 1]; // basename to match, e.g. "nfolders-open"
static QSocketNotifier *nf_watch_notifier = NULL;

// Reads and discards whatever inotify has queued, invoking nf_watch_cb once
// per matching event. Events for any other name in the watched directory are
// ignored -- matching by name rather than by watch descriptor, because a
// directory watch reports every file in it, not just the one asked for.
static void nf_watch_ready(int fd) {
    // inotify_event is variable-length: a name of up to NAME_MAX bytes
    // follows the fixed struct. This buffer is sized for several events at
    // once, per the batching `man 7 inotify` recommends, and aligned so the
    // struct can be read directly out of it.
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0)
        return;

    for (char *p = buf; p < buf + n; ) {
        struct inotify_event *ev = reinterpret_cast<struct inotify_event*>(p);
        // ev->name is NUL-terminated by the kernel (padded with NULs to
        // ev->len), so a plain strcmp is safe -- no QByteArray construction
        // per event, and no file-scope Qt object to compare against (see
        // nf_watch_name's own comment for why there cannot be one).
        if (ev->len && (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) &&
            strcmp(nf_watch_name, ev->name) == 0) {
            if (nf_watch_cb)
                nf_watch_cb();
        }
        p += sizeof(struct inotify_event) + ev->len;
    }
}

int nf_watch_init(char const *path, void (*cb)(void)) {
    QString qpath = QString::fromUtf8(path);
    int slash = qpath.lastIndexOf(QLatin1Char('/'));
    // qpath is always an absolute /tmp path in this mod, so slash >= 0 is not
    // a real possibility -- the fallback exists only so a malformed argument
    // fails safely (an empty dir string) rather than reading before index 0.
    QByteArray dir  = (slash >= 0 ? qpath.left(slash) : QStringLiteral(".")).toUtf8();
    // A byte copy into the POD buffer, not a QByteArray assignment -- see
    // nf_watch_name's declaration for why it is POD in the first place.
    // `dir` and `name` above/here are LOCAL QByteArrays, constructed at
    // runtime when this function executes, which is safe; the file-scope
    // object is the one that cannot have a constructor to run.
    QByteArray name = qpath.mid(slash + 1).toUtf8();
    snprintf(nf_watch_name, sizeof nf_watch_name, "%s", name.constData());

    // IN_NONBLOCK: this fd is driven entirely by the Qt event loop and must
    // never block the GUI thread. IN_CLOEXEC: it must not leak across a
    // future fork.
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        nh_log("watch: inotify_init1 failed: %s", strerror(errno));
        return -1;
    }

    // Watching the DIRECTORY, not the file: a watch cannot be established on
    // a path that does not exist yet, and the trigger file is created fresh
    // on every use (by `touch`, or NickelMenu), so it never exists ahead of
    // time.
    if (inotify_add_watch(fd, dir.constData(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        nh_log("watch: inotify_add_watch(%s) failed: %s", dir.constData(), strerror(errno));
        close(fd);
        return -1;
    }

    nf_watch_cb = cb;

    // Whether nf_init runs before or after QCoreApplication exists on THIS
    // firmware is unestablished -- the spike's poll thread never touched Qt
    // until trigger time, well after boot, so it never exercised this
    // ordering. QSocketNotifier needs the current thread's event dispatcher
    // to already be registered (which needs QCoreApplication to already
    // exist); if it is not, Qt warns and the notifier is silently never
    // registered -- a dead notifier that looks, from here, identical to a
    // working one. The device run has to answer this, not a guess in the
    // code, so both are logged loudly, and the notifier is still constructed
    // either way -- but the caller (nf_init) must NOT report readiness when
    // this is going to be dead on arrival, so the return value reflects the
    // dispatcher check, not just whether the syscalls above succeeded.
    if (!QCoreApplication::instance())
        nh_log("watch: QCoreApplication::instance() is NULL at nf_watch_init -- untested ordering, see nfnickel.cc");
    bool dispatcherLive = QAbstractEventDispatcher::instance() != NULL;
    if (!dispatcherLive)
        nh_log("watch: QAbstractEventDispatcher::instance() is NULL -- the notifier below will NOT fire, the trigger will silently never work");

    // No Q_OBJECT and no moc for anything of ours here, matching the house
    // style (see NFTrigger in nfolders.cc, before this rung): activated() is
    // already mocced inside Qt itself, so a plain functor connect needs
    // neither a QObject subclass nor a build-system moc step. The notifier
    // has no parent and is never freed -- a permanent, process-lifetime
    // singleton, like nf_proxy_owner above, not a per-event leak.
    nf_watch_notifier = new QSocketNotifier(fd, QSocketNotifier::Read);
    QObject::connect(nf_watch_notifier, &QSocketNotifier::activated,
                      [fd](int) { nf_watch_ready(fd); });

    // -1 here does not mean the notifier wasn't built -- it was, just above,
    // deliberately (see the comment on the dispatcher check). It means the
    // caller must not claim the watch is ready when it demonstrably is not:
    // nf_init logs "could not set up" and stays silent on "ready" rather than
    // printing a readiness line for a watch that already knows it is dead.
    return dispatcherLive ? 0 : -1;
}
