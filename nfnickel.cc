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
#include <QStringList>
#include <QVector>

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

// NOT static, unlike everything below this point -- nfbrowser.cc calls
// these directly to construct and push the real controller. nfnickel.h has
// the full rationale for each, including NF_BROWSER_USE_ARTICLE_LIST -- the
// device-run finding that made ArticleListLibraryController the one
// actually used, with QuickAccessLibraryController kept resolved only for
// comparison.
void  *(*MainWindowController__sharedInstance)(void);
void   (*MainWindowController__push)(MainWindowController *_this, void *controller, bool animate);
void   (*QuickAccessLibraryController__ctor)(void *_this, void const *source);
void   (*ArticleListLibraryController__ctor)(void *_this, void const *source);

// The native-dialog route's own symbols (nfnickel.h has the full derivation
// for each) -- NOT static, same reason as the four above: nfview.cc calls
// all of them directly.
void   (*MainWindowController__pushView)(MainWindowController *_this, QWidget *view);
void   (*MainWindowController__popView)(MainWindowController *_this, QWidget *view);
N3Dialog *(*N3DialogFactory__getDialog)(QWidget *content, bool fullScreenIdk);
void   (*N3Dialog__setTitle)(N3Dialog *_this, QString const &title);
void   (*N3Dialog__enableBackButton)(N3Dialog *_this, bool enable);
void   (*TouchLabel__ctor)(TouchLabel *_this, QWidget *parent, QFlags<Qt::WindowType> flags);

// Nickel's classes stay opaque, same discipline as Volume/ReadBookActionProxy
// above: these are the four classes the rung 2 data-source chain constructs,
// each typedef'd void and reached only through an explicitly written call
// signature. LibraryDataProvider and LibraryDataSource never appear as a
// `this` we construct directly -- they are only ever the STATIC TYPE of a
// QSharedPointer's value pointer -- but naming them keeps every signature
// below self-documenting about which QSharedPointer<...> it means.
typedef void LibraryDataProvider;
typedef void LibraryDataSource;

// QVector<Volume>::append(Volume const&) and ::~QVector() -- both static and
// private to this file (nf_build_volume_source, below, is the only caller;
// nfbrowser.cc never touches a QVector<Volume> itself). `vec` is always the
// address of an NFVolumeVector, defined just above nf_build_volume_source
// further down -- QVector<T>'s complete layout for every T is one Data* and
// nothing else (Qt 5.2's public qvector.h), so a raw `void*` argument here
// is exactly as precise as a real `QVector<Volume>*` would be, without
// needing Volume's real definition to declare one.
static void (*QVectorVolume__append)(void *vec, Volume const *v);
static void (*QVectorVolume__dtor)(void *vec);

// InMemoryDataProvider<Volume>::InMemoryDataProvider(QVector<Volume> const&)
// at 0x1082a90, and LinearLibraryDataSource<Volume>::LinearLibraryDataSource
// (QSharedPointer<LibraryDataProvider<Volume> >) at 0xaf88ac. Both static:
// only nf_build_volume_source calls them.
static void (*InMemoryDataProvider__ctor)(LibraryDataProvider *_this, void const *vec);
static void (*LinearLibraryDataSource__ctor)(LibraryDataSource *_this, void const *provider);

// QtSharedPointer::ExternalRefCountWithCustomDeleter<T, NormalDeleter>::deleter,
// for T = LibraryDataProvider<Volume> (0xc280f8) and T = LibraryDataSource<Volume>
// (0xb10920) -- the two DestroyerFn values nf_build_volume_source's hand-built
// ExternalRefCountData headers store, per the brief ("the one hand-built
// thing"). Never actually invoked (see nf_build_volume_source): both
// counters in every header this file builds are set high enough that no
// legitimate decrement chain reaches zero. Resolving these by name rather
// than guessing an address is still worth doing even though they are
// (deliberately) unreachable -- a resolved-but-unused pointer costs nothing,
// and NULL-gating nf_build_volume_source on them catches a firmware that
// dropped the symbol before it ever matters, not after.
static void (*NFRefCountDeleter_Provider)(void *refCountData);
static void (*NFRefCountDeleter_Source)(void *refCountData);

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
    {.name = "_ZN20MainWindowController14sharedInstanceEv",   .out = nh_symoutptr(MainWindowController__sharedInstance), .desc = "MainWindowController::sharedInstance", .optional = true},
    {.name = "_ZN20MainWindowController4pushEP18AbstractControllerb", .out = nh_symoutptr(MainWindowController__push), .desc = "MainWindowController::push",           .optional = true},
    {.name = "_ZN28QuickAccessLibraryControllerC1E14QSharedPointerI17LibraryDataSourceI6VolumeEE", .out = nh_symoutptr(QuickAccessLibraryController__ctor), .desc = "QuickAccessLibraryController::QuickAccessLibraryController", .optional = true},
    {.name = "_ZN28ArticleListLibraryControllerC1E14QSharedPointerI17LibraryDataSourceI6VolumeEE", .out = nh_symoutptr(ArticleListLibraryController__ctor), .desc = "ArticleListLibraryController::ArticleListLibraryController", .optional = true},
    {.name = "_ZN20MainWindowController8pushViewEP7QWidget", .out = nh_symoutptr(MainWindowController__pushView), .desc = "MainWindowController::pushView",         .optional = true},
    {.name = "_ZN20MainWindowController7popViewEP7QWidget",  .out = nh_symoutptr(MainWindowController__popView),   .desc = "MainWindowController::popView",          .optional = true},
    {.name = "_ZN15N3DialogFactory9getDialogEP7QWidgetb",    .out = nh_symoutptr(N3DialogFactory__getDialog),      .desc = "N3DialogFactory::getDialog",             .optional = true},
    {.name = "_ZN8N3Dialog8setTitleERK7QString",              .out = nh_symoutptr(N3Dialog__setTitle),              .desc = "N3Dialog::setTitle",                     .optional = true},
    {.name = "_ZN8N3Dialog16enableBackButtonEb",              .out = nh_symoutptr(N3Dialog__enableBackButton),      .desc = "N3Dialog::enableBackButton",             .optional = true},
    {.name = "_ZN10TouchLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE", .out = nh_symoutptr(TouchLabel__ctor),       .desc = "TouchLabel::TouchLabel",                 .optional = true},
    {.name = "_ZN7QVectorI6VolumeE6appendERKS0_",             .out = nh_symoutptr(QVectorVolume__append),           .desc = "QVector<Volume>::append",                .optional = true},
    {.name = "_ZN7QVectorI6VolumeED1Ev",                      .out = nh_symoutptr(QVectorVolume__dtor),             .desc = "QVector<Volume>::~QVector",              .optional = true},
    {.name = "_ZN20InMemoryDataProviderI6VolumeEC1ERK7QVectorIS0_E", .out = nh_symoutptr(InMemoryDataProvider__ctor), .desc = "InMemoryDataProvider<Volume>::InMemoryDataProvider", .optional = true},
    {.name = "_ZN23LinearLibraryDataSourceI6VolumeEC1E14QSharedPointerI19LibraryDataProviderIS0_EE", .out = nh_symoutptr(LinearLibraryDataSource__ctor), .desc = "LinearLibraryDataSource<Volume>::LinearLibraryDataSource", .optional = true},
    {.name = "_ZN15QtSharedPointer33ExternalRefCountWithCustomDeleterI19LibraryDataProviderI6VolumeENS_13NormalDeleterEE7deleterEPNS_20ExternalRefCountDataE", .out = nh_symoutptr(NFRefCountDeleter_Provider), .desc = "ExternalRefCountWithCustomDeleter<LibraryDataProvider<Volume>>::deleter", .optional = true},
    {.name = "_ZN15QtSharedPointer33ExternalRefCountWithCustomDeleterI17LibraryDataSourceI6VolumeENS_13NormalDeleterEE7deleterEPNS_20ExternalRefCountDataE", .out = nh_symoutptr(NFRefCountDeleter_Source), .desc = "ExternalRefCountWithCustomDeleter<LibraryDataSource<Volume>>::deleter", .optional = true},
    {0},
};

bool nf_nickel_resolve(void) {
    return VolumeManager__getById && Volume__isValid && Volume__dtor &&
           ReadBookActionProxy__ctor && ReadBookActionProxy__onSelected &&
           Device__getCurrentDevice && Device__getDbName;
}

// A SEPARATE gate from nf_nickel_resolve() above, deliberately: rung 2's
// screen needs none of the getById/Volume/ReadBookActionProxy symbols
// directly (nf_build_volume_source below calls nf_nickel_resolve() itself
// for the getById step it DOES need), and book-opening needs none of these
// eight. Folding them into one bool would mean a firmware that renames just
// MainWindowController::push, say, also disables book-opening for no
// reason -- exactly the unnecessary coupling CLAUDE.md's "treat anything
// non-essential as non-fatal" argues against. Two independent bools keep the
// two features' failure domains independent, the same way each is already
// independently .optional in the table above.
bool nf_browser_resolve(void) {
    if (!(MainWindowController__sharedInstance && MainWindowController__push &&
          QVectorVolume__append && QVectorVolume__dtor &&
          InMemoryDataProvider__ctor && LinearLibraryDataSource__ctor &&
          NFRefCountDeleter_Provider && NFRefCountDeleter_Source))
        return false;

    // Only the controller NF_BROWSER_USE_ARTICLE_LIST actually selects
    // (nfnickel.h) needs to have resolved -- the other one is kept resolved
    // for a possible comparison rebuild (see its own comment), but is not
    // on this build's critical path, so its absence must not disable the
    // browser screen.
#if NF_BROWSER_USE_ARTICLE_LIST
    return ArticleListLibraryController__ctor != NULL;
#else
    return QuickAccessLibraryController__ctor != NULL;
#endif
}

// A fourth independent gate, for the native-dialog route (nfview.cc) only
// -- see nfnickel.h's own comment on each of these six for what they are.
// Kept disjoint from the other three (nf_nickel_resolve, nf_browser_resolve)
// for the same reason those two stay disjoint from each other: a firmware
// that renames, say, TouchLabel's constructor must not also disable
// book-opening or the borrowed-controller browser screen. A cheap,
// side-effect-free predicate nf_init can log at boot, matching
// nf_nickel_resolve/nf_browser_resolve's own pattern.
bool nf_native_view_resolve(void) {
    return MainWindowController__sharedInstance && MainWindowController__pushView &&
           MainWindowController__popView && N3DialogFactory__getDialog &&
           N3Dialog__setTitle && N3Dialog__enableBackButton && TouchLabel__ctor;
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

// --- rung 2: the data-source chain -----------------------------------------
//
// QVector<T>'s complete runtime layout, for every T, per Qt 5.2's public
// qvector.h: one implicitly-shared Data* and nothing else -- 4 bytes. A
// default-constructed QVector<T>'s Data* is Qt5Core's own universal empty
// sentinel (QArrayData::shared_null[0]); QTypedArrayData<T>::sharedNull() is
// nothing but a reinterpret_cast of that ONE un-templated global, so its
// ADDRESS does not depend on T at all. nf_qvector_shared_null(), below,
// exploits exactly that: a real, host-legal QVector<int> -- int needs no
// opacity, it is not one of Nickel's own classes -- gives us, by
// construction, the identical bit pattern an unreachable QVector<Volume>'s
// own default constructor would produce, without declaring any private Qt
// struct and without needing Volume's real definition. This is public Qt5
// architecture (the implicit-sharing empty-sentinel pattern), not a guess:
// libnickel.so.1.0.0 itself imports _ZN10QArrayData11shared_nullE as an
// UNDEFINED symbol (readelf -D -r), i.e. from Qt5Core, so every QVector<T>
// in this whole process -- Nickel's and ours alike -- already shares this
// one value.
struct NFVolumeVector { void *d; };

static void *nf_qvector_shared_null() {
    QVector<int> probe;
    return *reinterpret_cast<void* const*>(&probe);
}

// Qt 5.2's public qsharedpointer_impl.h: ExternalRefCountData is FOUR
// words, not three -- caught in review, and confirmed directly against the
// one place Nickel's own code builds exactly this pair:
// QuickAccessMenuView::QuickAccessMenuView (0xf49850-0xf498c4 on
// 4.38.23684). `movs r0,#16` at 0xf498a2, `blx operator new`, then
// `str.w r9,[r3,#12]` at 0xf498b2 writes the FOURTH word -- the managed T*
// pointer -- into the freshly-allocated block, followed immediately by
// `[+0]=1, [+4]=1, [+8]=deleter`. **Field order matters, and the first
// draft had it backwards**: both
// `ExternalRefCountWithCustomDeleter<T,NormalDeleter>::deleter` functions
// (0xc280f8 for the provider, 0xb10920 for the source) begin with
// `ldr r?,[r0,#12]` -- confirmed by direct disassembly of both -- reading
// the managed pointer this struct stores at +12, not their own `this` at
// some other offset; and
// `QSharedPointer<LinearLibraryDataSource<Volume>>::deref` (0x1082bd8)
// decrements [+4] FIRST, and [+4] reaching zero is what invokes `[+8](d)`
// (the deleter) -- which only makes sense if [+4] is **strongref**, the
// managed object's own count. [+0] is decremented SECOND, unconditionally,
// and [+0] reaching zero is what tail-calls `operator delete(d)` to free
// the header itself -- so [+0] is **weakref**, per Qt5's real declaration
// order (weakref before strongref). Getting this backwards was harmless
// only because both counters happen to be set to the identical value below
// -- a future edit that set them differently would silently corrupt this.
//
// THE ONE hand-built structure this rung allows itself (CLAUDE.md; the
// task brief's "the one hand-built thing"): Nickel exports no way to
// construct a strong QSharedPointer's control block, only the QObject
// weak-pointer helpers. Allocated as 64 bytes, not the measured 16, and
// zeroed first -- CLAUDE.md's "over-allocate for every Nickel constructor"
// rule, extended to this hand-built object even though nothing here is a
// literal Nickel constructor call; the alternative (allocating exactly 16)
// was the one exception to that rule in this file, and review correctly
// called it out as needing the same margin as everything else.
struct NFRefCountData {
    int   weakref;
    int   strongref;
    void  (*destroyer)(void *refCountData);
    void  *managed;
};

bool nf_build_volume_source(QStringList const& contentIds, QString const& dbName, NFSharedPtr *outSource, int *outKept) {
    *outKept = 0;

    if (!nf_nickel_resolve()) {
        nh_log("browser: a getById-path symbol never resolved, refusing to build a data source");
        return false;
    }
    if (!QVectorVolume__append || !QVectorVolume__dtor || !InMemoryDataProvider__ctor ||
        !LinearLibraryDataSource__ctor || !NFRefCountDeleter_Provider || !NFRefCountDeleter_Source) {
        nh_log("browser: a data-source symbol never resolved, refusing");
        return false;
    }

    NFVolumeVector vec = { nf_qvector_shared_null() };

    int kept = 0;
    for (int i = 0; i < contentIds.size(); i++) {
        // Same buffer size, same getById/isValid/dtor discipline as
        // nf_open_book_staged -- see that function's own comments for the
        // measurements behind volbuf's size, and for why the dtor is called
        // ONLY when v is non-null: a null v means getById did not construct
        // into volbuf, so there is nothing to destroy (this mirrors
        // nf_open_book_staged's own `if (!v) return false;` guard exactly,
        // rather than introducing a second, looser discipline here). An
        // invalid-but-constructed ContentID's Volume is simply never
        // appended: this IS the negative control the brief's device
        // checklist asks for (a row for a ContentID no book has must be
        // absent, not crash), and it needs no special-casing beyond that.
        unsigned char volbuf[128] __attribute__((aligned(8)));
        memset(volbuf, 0, sizeof volbuf);
        Volume *v = VolumeManager__getById(volbuf, &contentIds.at(i), &dbName);
        if (!v)
            continue;
        if (Volume__isValid(v)) {
            QVectorVolume__append(&vec, v);
            kept++;
        }
        Volume__dtor(v);
    }
    *outKept = kept;
    nh_log("browser: %d of %d ContentIDs resolved to a book", kept, static_cast<int>(contentIds.size()));

    if (kept == 0) {
        // A caller cannot otherwise distinguish "the chain is broken" from
        // "the reference list is stale" -- the primary device test is
        // `touch /tmp/nfolders-show` against 28 hardcoded filenames off one
        // card (nfolders.cc), and if those have moved, silently pushing an
        // empty screen would look identical to a real failure. Refuse
        // instead: nothing has been allocated yet at this point (`vec`
        // holds only the shared-null sentinel, never own storage), so
        // there is nothing to clean up on this path.
        nh_log("browser: no ContentID resolved to a book, refusing to build an empty screen");
        return false;
    }

    // sizeof(InMemoryDataProvider<Volume>) == 16, sizeof(LinearLibraryDataSource
    // <Volume>) == 12: BOTH corrected in review from an earlier claim that
    // neither had any real Nickel allocation site in this firmware. That
    // claim was wrong, and the reason is worth recording so it is not
    // repeated: `readelf -r` TRUNCATES the symbol column (confirmed:
    // `_ZN20InMemoryDataProviderI6VolumeEC1E...` prints as
    // `_ZN20InMemoryDataProvi` and nothing past it), so a grep for either
    // constructor's FULL mangled name matched nothing and the sweep
    // silently reported empty -- a sweep that cannot fail visibly is not a
    // sweep. `objdump -R` does not truncate and finds both:
    // `016b7b98 R_ARM_JUMP_SLOT _ZN20InMemoryDataProviderI6VolumeEC1E...`
    // and `016ad074 R_ARM_JUMP_SLOT _ZN23LinearLibraryDataSourceI6VolumeEC1E...`,
    // with PLT stubs at 0x699820 and 0x67775c respectively (`tools/plt.sh`
    // resolves both), each with real callers in `.text`.
    //
    // The two sizes themselves, each read at a genuine Nickel `operator
    // new` call site, the strongest measurement this project uses:
    //   - InMemoryDataProvider<Volume>: `movs r0,#16` at **0x1081112**,
    //     inside `ShelfListBuilder::refresh()`, immediately before
    //     `blx operator new` then `blx 0x699820` (this exact ctor).
    //   - LinearLibraryDataSource<Volume>: `movs r0,#12` at **0xf498b8**,
    //     inside `QuickAccessMenuView::QuickAccessMenuView` -- the SAME
    //     function QuickAccessLibraryController's own sizeof(72) was read
    //     from (nfnickel.h) -- immediately before `blx operator new` then
    //     `blx 0x67775c` (this exact ctor).
    // Both exactly match this file's own earlier LOWER-BOUND derivation
    // (read from each constructor's own writes to `this`, the technique
    // CLAUDE.md's AbstractController precedent established for when no
    // call site can be found): the lower bounds were 16 and 12, and the
    // real, Nickel-measured sizes are 16 and 12. Still over-allocated to
    // 256 below -- CLAUDE.md's margin, not a reaction to any remaining
    // doubt about the number itself.
    void *providerBuf = ::operator new(256);
    memset(providerBuf, 0, 256);
    InMemoryDataProvider__ctor(providerBuf, &vec);

    // Our own reference is no longer needed: InMemoryDataProvider's ctor
    // refcount-shares the vector's Data* (read directly in its own
    // disassembly -- an atomic increment on the incoming vector's `d`,
    // not a deep copy) rather than deep-copying, so destroying our local
    // copy here does not free the elements -- it just drops the ONE
    // reference we were holding. This is real cleanup, the same
    // Volume__dtor discipline as the loop above, one level up -- not part
    // of the deliberate leak below.
    QVectorVolume__dtor(&vec);

    // strongref = weakref = 2, not the real ctor's 1 -- and this is an
    // EXACT, load-bearing value, not merely "high enough." Both
    // LinearLibraryDataSource's and QuickAccessLibraryController's own
    // ctors take their QSharedPointer argument BY VALUE (neither mangled
    // name carries `RK` -- NFSharedPtr's own comment, nfnickel.h), which
    // means the CALLEE destroys what it was handed, not the caller. Traced
    // through both ctors' disassembly: starting from 2, +1 for a local
    // copy the ctor makes for itself, +1 more for an in-place bump as that
    // copy is handed onward, by value again, to the base ctor's own
    // parameter; the base ctor itself does +1 as it copies the parameter
    // into its own stored member and -1 as its now-redundant parameter
    // copy is destroyed; then -1 as the caller's local copy unwinds, and
    // -1 as the original by-value argument (ours) is destroyed by the
    // callee per the by-value calling convention above -- net back to
    // exactly 2, unchanged, by the time construction returns. The ONE
    // decrement this chain still owes happens far later, when Nickel
    // itself eventually destroys the controller: THAT is what brings the
    // count to 1. **Not 0 -- if this started at 1 instead of 2, that final
    // decrement would reach zero and fire the deleter on a controller
    // still nominally in use.** So 2 is the smallest correct value, not an
    // arbitrary safety margin, and NEITHER counter may be lowered without
    // re-deriving this chain.
    //
    // A deliberate, permanent leak of one 64-byte header per QSharedPointer
    // this function builds (two per screen: one for the provider, one for
    // the source), same treatment as the leaked InMemoryDataProvider/
    // LinearLibraryDataSource/QuickAccessLibraryController objects
    // themselves (never freed for the same reason: the deleter that would
    // free them is what these headers exist to permanently suppress).
    // CLAUDE.md asks a deliberate leak to carry the reason so nobody
    // deletes it as a bug: the alternative is letting Qt call a deleter on
    // memory whose ownership this mod cannot prove.
    NFRefCountData *providerRef = static_cast<NFRefCountData*>(::operator new(64));
    memset(providerRef, 0, 64);
    providerRef->weakref   = 2;
    providerRef->strongref = 2;
    providerRef->destroyer = NFRefCountDeleter_Provider;
    providerRef->managed   = providerBuf;

    NFSharedPtr sp_provider = { providerBuf, providerRef };

    void *sourceBuf = ::operator new(256);
    memset(sourceBuf, 0, 256);
    LinearLibraryDataSource__ctor(sourceBuf, &sp_provider);

    NFRefCountData *sourceRef = static_cast<NFRefCountData*>(::operator new(64));
    memset(sourceRef, 0, 64);
    sourceRef->weakref   = 2;
    sourceRef->strongref = 2;
    sourceRef->destroyer = NFRefCountDeleter_Source;
    sourceRef->managed   = sourceBuf;

    outSource->value = sourceBuf;
    outSource->d      = sourceRef;
    return true;
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

// Rung 2 needs a SECOND trigger (nfolders.cc: /tmp/nfolders-show, alongside
// the existing /tmp/nfolders-open), and both live directly in /tmp -- so
// this is now a small table of (basename -> callback) pairs sharing ONE
// inotify fd/watch/notifier on that one directory, rather than the single
// (name, callback) pair rung 1 needed. NF_WATCH_MAX_ENTRIES is a small fixed
// cap, not a QVector, for the same POD-at-file-scope reason as the buffer
// below -- see its comment.
#define NF_WATCH_MAX_ENTRIES 4

struct NFWatchTarget {
    char name[NAME_MAX + 1]; // basename to match, e.g. "nfolders-open"
    void (*cb)(void);
};

// POD, NOT QByteArray/QVector, and this is load-bearing, not style: a
// file-scope QByteArray here previously segfaulted Nickel on every boot.
// NickelHook's __attribute__((constructor)) nh_init runs from THIS library's
// .init_array before this translation unit's own C++ dynamic initialiser
// (_GLOBAL__sub_I_nfnickel.cc) does -- confirmed with readelf, see the fix
// report -- and nh_init calls straight through nf_init -> nf_watch_init
// before that initialiser has run. A QByteArray assignment there dereferenced
// a `d` pointer that had never been constructed (still zeroed .bss), which is
// a load from address 0. POD types are always zero-initialised by .bss
// itself, with no constructor to race against, which is why NO file-scope
// object in this translation unit may have a non-trivial (dynamically
// initialised) constructor: this is the only kind of global whose
// initialisation order relative to nh_init is not guaranteed. Every other
// static above and below this line is POD for exactly this reason -- do not
// add a QString/QByteArray/QObject/QVector/etc. at file scope.
static NFWatchTarget    nf_watch_targets[NF_WATCH_MAX_ENTRIES];
static int              nf_watch_target_count = 0;
static char             nf_watch_dir[PATH_MAX]; // the one directory this mod watches; set by the first call
static QSocketNotifier *nf_watch_notifier = NULL;

// Reads and discards whatever inotify has queued, invoking the matching
// target's callback once per matching event. An event whose name matches
// none of nf_watch_targets is ignored -- matching by name rather than by
// watch descriptor, because a directory watch reports every file in it, not
// just the ones asked for.
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
        // The advance to the NEXT event is computed HERE, unconditionally,
        // before the filter test below, and the filter/dispatch is wrapped
        // in an `if` rather than an early `continue` -- an earlier version
        // of this loop used `continue` for the filtered-out case with the
        // advance written as the body's LAST statement, which a `for` with
        // no increment clause turns into an infinite loop: `continue` skips
        // straight past the advance and re-tests `p < buf + n` against the
        // SAME `p`. This is not a corner case: IN_Q_OVERFLOW, IN_IGNORED and
        // IN_UNMOUNT are all delivered regardless of the requested mask, and
        // all carry len == 0, so the very first such event would hang this
        // loop. It runs on Nickel's GUI thread, so the failure mode is not a
        // crash -- it is Nickel's UI thread locking up solid, PID unchanged,
        // invisible to tools/nftest.sh's PID-change abort, recoverable only
        // by a power cycle. Do not reintroduce a `continue` here without
        // also moving the advance ahead of it again.
        char *next = p + sizeof(struct inotify_event) + ev->len;
        // ev->name is NUL-terminated by the kernel (padded with NULs to
        // ev->len), so a plain strcmp is safe -- no QByteArray construction
        // per event, and no file-scope Qt object to compare against (see
        // nf_watch_targets' own comment for why there cannot be one).
        if (ev->len && (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) {
            for (int i = 0; i < nf_watch_target_count; i++) {
                if (strcmp(nf_watch_targets[i].name, ev->name) == 0 && nf_watch_targets[i].cb) {
                    nf_watch_targets[i].cb();
                    break; // basenames are unique by construction (one nf_watch_init call each)
                }
            }
        }
        p = next;
    }
}

int nf_watch_init(char const *path, void (*cb)(void)) {
    QString qpath = QString::fromUtf8(path);
    int slash = qpath.lastIndexOf(QLatin1Char('/'));
    // qpath is always an absolute /tmp path in this mod, so slash >= 0 is not
    // a real possibility -- the fallback exists only so a malformed argument
    // fails safely (an empty dir string) rather than reading before index 0.
    QByteArray dir  = (slash >= 0 ? qpath.left(slash) : QStringLiteral(".")).toUtf8();
    // `dir` and `name` are LOCAL QByteArrays, constructed at runtime when
    // this function executes, which is safe; the file-scope objects are the
    // ones that cannot have a constructor to run -- see nf_watch_targets'
    // comment above.
    QByteArray name = qpath.mid(slash + 1).toUtf8();

    if (nf_watch_target_count >= NF_WATCH_MAX_ENTRIES) {
        nh_log("watch: already tracking %d watches (the fixed cap), refusing %s", NF_WATCH_MAX_ENTRIES, path);
        return -1;
    }

    bool firstWatch = (nf_watch_notifier == NULL);

    if (firstWatch) {
        snprintf(nf_watch_dir, sizeof nf_watch_dir, "%s", dir.constData());

        // IN_NONBLOCK: this fd is driven entirely by the Qt event loop and
        // must never block the GUI thread. IN_CLOEXEC: it must not leak
        // across a future fork.
        int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0) {
            nh_log("watch: inotify_init1 failed: %s", strerror(errno));
            return -1;
        }

        // Watching the DIRECTORY, not the file: a watch cannot be
        // established on a path that does not exist yet, and every trigger
        // file this mod uses is created fresh on every use (by `touch`, or
        // NickelMenu), so it never exists ahead of time.
        if (inotify_add_watch(fd, dir.constData(), IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
            nh_log("watch: inotify_add_watch(%s) failed: %s", dir.constData(), strerror(errno));
            close(fd);
            return -1;
        }

        // No Q_OBJECT and no moc for anything of ours here, matching the
        // house style (see NFTrigger in nfolders.cc, before rung 1):
        // activated() is already mocced inside Qt itself, so a plain functor
        // connect needs neither a QObject subclass nor a build-system moc
        // step. The notifier has no parent and is never freed -- a
        // permanent, process-lifetime singleton, like nf_proxy_owner above,
        // not a per-event leak.
        nf_watch_notifier = new QSocketNotifier(fd, QSocketNotifier::Read);
        QObject::connect(nf_watch_notifier, &QSocketNotifier::activated,
                          [fd](int) { nf_watch_ready(fd); });
    } else if (strcmp(nf_watch_dir, dir.constData()) != 0) {
        // Every trigger file this mod uses lives directly in /tmp, so this
        // is not a real limitation in practice -- but it is checked rather
        // than silently watching the wrong directory for a second trigger
        // that happens to live elsewhere.
        nh_log("watch: %s is in '%s', already watching '%s' -- a second directory is not supported, refusing",
               path, dir.constData(), nf_watch_dir);
        return -1;
    }

    snprintf(nf_watch_targets[nf_watch_target_count].name,
             sizeof nf_watch_targets[nf_watch_target_count].name, "%s", name.constData());
    nf_watch_targets[nf_watch_target_count].cb = cb;
    nf_watch_target_count++;

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
    // dispatcher check, not just whether the syscalls above succeeded. This
    // check runs on EVERY call (not just firstWatch), because it is this
    // CALL's caller that needs an honest answer, even when the underlying
    // notifier was already built for an earlier target.
    if (!QCoreApplication::instance())
        nh_log("watch: QCoreApplication::instance() is NULL at nf_watch_init -- untested ordering, see nfnickel.cc");
    bool dispatcherLive = QAbstractEventDispatcher::instance() != NULL;
    if (!dispatcherLive)
        nh_log("watch: QAbstractEventDispatcher::instance() is NULL -- the notifier below will NOT fire, the trigger will silently never work");

    // -1 here does not mean the notifier wasn't built -- it was, on the
    // firstWatch branch above (or on an earlier call, for a reused watch),
    // deliberately (see the comment on the dispatcher check). It means the
    // caller must not claim the watch is ready when it demonstrably is not:
    // nf_init logs "could not set up" and stays silent on "ready" rather than
    // printing a readiness line for a watch that already knows it is dead.
    return dispatcherLive ? 0 : -1;
}
