// Rung 2: a screen of ours on Nickel's own window stack.
//
// AbstractController is not a QObject (nfnickel.h), so there is no
// metaobject to fake and no real C++ class of ours to derive from it --
// per CLAUDE.md's opaque-classes rule, this file builds a raw object with
// AbstractController's layout and a vtable that is Nickel's own with one
// slot replaced, exactly the way every one of Nickel's own controllers
// builds itself (NOTES.md, "The two vtable writes, and what it means for
// us"). Getting this wrong crashes Nickel, not just this mod -- see
// CLAUDE.md's "Method: adding a new libnickel call" and NOTES.md's account
// of VolumeManager::getById's missing `this`, the last time a guess here
// went unchecked.

#include "nfbrowser.h"
#include "nfnickel.h"

#include <QString>
#include <QVariant>
#include <QWidget>

#include <string.h> // memset

#include <NickelHook.h>

// Nickel's _ZTV18AbstractController, read live and copied wholesale rather
// than hand-transcribed: an 11-word array (offset-to-top, RTTI pointer,
// then the 9 slots CLAUDE.md's measured table lists). This copy is built
// ONCE, lazily, on first use, and is never freed -- a permanent,
// process-lifetime table, same pattern as nf_proxy_owner/nf_watch_notifier
// in nfnickel.cc -- so it outlives every controller whose vptr points into
// it, including ones pushed long after this function last ran. POD, not a
// QVector or similar: no file-scope object here may have a dynamic
// initialiser (CLAUDE.md), and this one does not need one -- .bss zeroes it,
// and nf_browser_vtable_ready() below fills it in at runtime.
static void *NFControllerVTable[11];

// The Itanium address point (the value a real vptr holds) is the table
// base plus 8 bytes -- two header words -- confirmed independently by
// AbstractController::AbstractController's own `adds r3, #8` before it
// stores the vptr, AND by ensureViewLoaded's `ldr r3, [r3, #32]` for slot 8
// landing exactly on _ZTV18AbstractController's __cxa_pure_virtual
// relocation at that same +32 offset from the same base. NFControllerVTable
// mirrors that: index 0/1 are the header words, indices 2..10 are slots
// 0..8, and &NFControllerVTable[2] is what gets stored as a vptr below.
enum { NF_VTABLE_HEADER_WORDS = 2, NF_VTABLE_SLOT8_INDEX = NF_VTABLE_HEADER_WORDS + 8 };

static void nf_browser_load_view(void *self);

// Populates NFControllerVTable from the live, already-relocated table on
// first call; every later call is a no-op (slot 2, `size()`, is never NULL
// once real once set, since it is copied straight from a resolved,
// non-NULL Nickel pointer). Returns false only if AbstractController__vtable
// itself never resolved -- nf_browser_resolve() already covers that for
// nf_browser_show()'s own gate, so this mirrors it rather than trusting the
// caller to have checked, since a future caller of this function might not.
static bool nf_browser_vtable_ready() {
    if (NFControllerVTable[2])
        return true;
    if (!AbstractController__vtable)
        return false;

    for (int i = 0; i < NF_VTABLE_SLOT8_INDEX; i++)
        NFControllerVTable[i] = AbstractController__vtable[i];
    // The ONE slot Nickel's own table leaves as __cxa_pure_virtual --
    // CLAUDE.md's measured table calls it out as "the one we must supply".
    // Every other slot above keeps Nickel's own implementation verbatim:
    // size(), viewWillAppear(), viewWillDisappear(), viewWillBeDestroyed(),
    // allowedOrientations() and navSection() are real code we want, not
    // reimplemented here.
    NFControllerVTable[NF_VTABLE_SLOT8_INDEX] = reinterpret_cast<void*>(&nf_browser_load_view);
    return true;
}

// Slot 8 -- Nickel's own vtable has __cxa_pure_virtual here.
// AbstractController::ensureViewLoaded (0xad1408 on 4.38.23684) calls it
// with `this` in r0 through vtable+32 and DISCARDS whatever it returns (the
// very next instruction it executes overwrites r0 before reading it), so
// void is the correct, not merely convenient, return type -- there is no
// return-value contract to honour. NOTES.md, "AbstractController::
// ensureViewLoaded calls slot 8...", has the disassembly this rests on.
//
// Its entire observable contract, read off that same caller, is: leave a
// valid QWidget* at this+8. AbstractController::viewLoaded() (0xad13ac)
// answers from exactly that offset (`ldr r3, [r0, #8]`), and this rung
// draws nothing on purpose, so an otherwise-default QWidget is exactly what
// "loaded" should mean here.
//
// The "mainNavView" property is not cosmetic.
// MainWindowController::currentViewName() (0xea8010) -- what NickelDBus's
// ndbCurrentView, this project's oracle, almost certainly calls, given the
// name match and that it returns exactly the class-name-shaped strings
// NickelDBus is documented to report (HomePageView, ReadingView, ...) --
// reads QStackedWidget::currentWidget(), then
// widget->property("mainNavView").toString() (that literal string was read
// directly out of the binary, not guessed -- NOTES.md), and only falls back
// to widget->metaObject()->className() if that property is empty. A bare
// QWidget's className() is just "QWidget" -- distinct enough to prove a
// non-baseline screen appeared even if this reading of currentViewName() is
// wrong, which is why both are covered rather than leaning on either alone.
static void nf_browser_load_view(void *self) {
    QWidget *view = new QWidget();
    view->setProperty("mainNavView", QStringLiteral("NFBrowserView"));

    // AbstractController::AbstractController() zeroes this offset, and
    // nothing else in the vtable slots copied above writes it -- CLAUDE.md's
    // measured table and NOTES.md both place the view pointer here. A raw
    // offset write, not a redeclared method or struct member, per the
    // opaque-classes rule: AbstractController stays `typedef void`.
    *reinterpret_cast<QWidget**>(static_cast<char*>(self) + 8) = view;
}

void nf_browser_show(void) {
    if (!nf_browser_resolve()) {
        nh_log("browser: a required libnickel symbol never resolved, refusing");
        return;
    }
    if (!nf_browser_vtable_ready()) {
        // Unreachable given the check just above (nf_browser_resolve()
        // already requires AbstractController__vtable to be non-NULL), but
        // this function does not trust that a future caller checked, so it
        // checks again rather than dereferencing blind.
        nh_log("browser: could not build the controller vtable, refusing");
        return;
    }

    void *mwc = MainWindowController__sharedInstance();
    if (!mwc) {
        nh_log("browser: MainWindowController::sharedInstance() returned null, refusing");
        return;
    }

    // sizeof(AbstractController) == 12 bytes on 4.38.23684 (NOTES.md,
    // triple-checked: the constructor's own three word-writes, and two
    // independent derived controllers each starting their own next field
    // exactly 12 bytes after their AbstractController base). Over-allocated
    // by more than 20x, per CLAUDE.md's "over-allocate for every Nickel
    // constructor" -- the constructor cannot be told how much room it has,
    // so this headroom is what survives a firmware that grows the object.
    void *controller = ::operator new(256);
    memset(controller, 0, 256);

    // Sets this[+0] (a Nickel vtable pointer, momentarily), zeroes
    // this[+4] and this[+8] -- the same three writes AbstractController's
    // own constructor always makes, on Nickel's own controllers too.
    AbstractController__ctor(controller);

    // ...and immediately overwrite that vptr with OUR copy (slot 8
    // replaced), exactly the pattern every derived Nickel controller uses
    // on itself: the base constructor sets the base vtable pointer, the
    // derived constructor immediately re-sets it to its own combined
    // vtable. Confirmed reading PasswordController's and
    // HelpDialogController's own constructors (NOTES.md) -- not invented
    // for this mod. this[+4] and this[+8] are left exactly as
    // AbstractController::AbstractController set them (both zero), which is
    // also what every real controller's OWN constructor leaves them as at
    // this same point in construction.
    *reinterpret_cast<void***>(controller) = &NFControllerVTable[NF_VTABLE_HEADER_WORDS];

    nh_log("browser: pushing an empty screen");
    // `true`: animate the transition in, matching what a user-tap-driven
    // push looks like elsewhere in Nickel's own UI -- NOT independently
    // established what this bool controls beyond "passed straight through
    // to MainWindowController::unprepareView() on whatever was on top
    // before" (NOTES.md); a cosmetic transition flag, not a
    // correctness-affecting one, going by everywhere it is used in push()'s
    // own disassembly.
    MainWindowController__push(mwc, controller, true);
}
