// Our own screen, built out of Nickel's own dialog chrome (N3Dialog) and
// Nickel's own tappable row widget (TouchLabel) -- see nfview.h and
// NOTES.md's "Task 8: touch input archaeology" for why this replaces the
// earlier AbstractController shim.
//
// THE SHORT VERSION of that archaeology, because it explains every choice
// below: Nickel does not deliver QMouseEvents. It reads the touch panel
// itself, injects QTouchEvents, and recognises its own gestures through six
// custom QGestureRecognizer subclasses. A bare QWidget/QPushButton needs
// THREE things to receive a tap -- a grabGesture() call against a
// registration-time token, an event() override that routes touch/gesture
// events (QWidget::event() does neither), and RTTI-based GestureDelegate
// dispatch -- and the earlier shim's QPushButton, a stock Qt widget, had
// none of them: it rendered correctly and never once received a tap. Rather
// than hand-build any of that (or hand-build the AbstractController-shaped
// object the previous rung needed just to get onto the window stack at
// all), this route uses Nickel's OWN widgets, which already do it:
// TouchLabel self-registers for gestures in its own constructor, and
// N3Dialog is the screen chrome Nickel's own library search/settings/etc.
// dialogs use. Nothing here fabricates a vtable, a cross-cast, or RTTI for
// a class of our own -- every Nickel type stays opaque (typedef void +
// explicit call signature, nfnickel.h), the same discipline as every other
// libnickel entry point in this project.
//
// Prior art, read but not disassembled (both MIT): NickelHardcover
// (codeberg.org/StrayRose/NickelHardcover, HEAD 2026-07-21) is where the
// getDialog/pushView shape and the `true` argument to getDialog come from --
// its own comment does not explain that bool either, and neither does this
// one; it is copied working behaviour, not an understood value. NickelMenu
// (pgaskin/NickelMenu) is where the hidden-QPushButton signal-adaptor trick
// (below) comes from, for reaching a lambda without this project's having
// any moc step of its own.

#include "nfview.h"
#include "nfnickel.h"

#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <stdlib.h> // calloc -- see the row-allocation comment below for why not ::operator new

#include <NickelHook.h>

// --- casting an opaque Nickel pointer to a REAL, linked Qt base class -----
//
// TouchLabel (via FontSizeAdjustingLabel) inherits QLabel, and N3Dialog
// (via QDialog) inherits QWidget/QObject -- NOTES.md. Both chains are
// ordinary Qt single inheritance (no virtual bases ahead of QObject in any
// Qt widget hierarchy), so a TouchLabel* or N3Dialog* IS, bit for bit, a
// valid pointer to that base subobject -- the same "no cast arithmetic
// needed" property nfnickel.h already documents for
// QuickAccessLibraryController's own base. Calling QLabel::setText,
// QWidget::addWidget-compatible use, or QObject::setObjectName/connect
// through such a cast is NOT a redeclaration of anything Nickel's own
// compiler generated for TouchLabel/N3Dialog specifically -- it is calling
// an ordinary, ABI-stable, already-linked Qt5Widgets function (this
// project already links Qt5Widgets, Makefile) on a subobject whose
// position is guaranteed by Qt's own inheritance conventions, not by
// anything measured about Nickel's internals. What stays strictly opaque
// is everything Nickel-specific: TouchLabel's own constructor, N3Dialog's
// own setTitle/enableBackButton/getDialog, all resolved and called through
// nfnickel.h's usual function pointers, never assumed or redeclared.

// --- construction ------------------------------------------------------

// Pops this screen and logs why. Shared by both exit paths it offers (the
// guaranteed BACK row and N3Dialog's own backTapped() signal, both below)
// so a reader can see in one place that they really do the same thing,
// rather than auditing two near-duplicate lambdas for drift. `why` is
// always a string literal from the call site, never built here.
//
// popView DESTROYS the widget (setVisible(false), deleteLater(),
// stack->removeWidget() -- NOTES.md) -- including, eventually, `content`
// and everything under it, the very button whose clicked signal is what
// got this function called. Safe from inside that signal handler because
// deleteLater() only POSTS the deletion for later in the event loop
// rather than deleting synchronously -- the standard Qt "a slot may
// schedule its own object's death" pattern, not a hazard specific to this
// call.
static void nf_pop_native_view(void *mwc, N3Dialog *dialog, char const *why) {
    nh_log("view: %s, popping", why);
    MainWindowController__popView(mwc, reinterpret_cast<QWidget*>(dialog));
}

bool nf_browser_show(void) {
    if (!nf_native_view_resolve()) {
        nh_log("view: a required libnickel symbol did not resolve, refusing");
        return false;
    }

    void *mwc = MainWindowController__sharedInstance();
    if (!mwc) {
        nh_log("view: MainWindowController::sharedInstance() returned null, refusing");
        return false;
    }

    // `content` is OURS, start to finish -- a plain Qt5Widgets QWidget, not
    // one of Nickel's classes, so ordinary `new`/Qt-ownership rules apply
    // with no opacity concerns at all. It is never deleted by this
    // function: N3DialogFactory::getDialog's own setContent(content) call,
    // below, reparents it into the dialog's layout and calls
    // content->show() (NOTES.md), and from that point on Qt's own
    // parent/child teardown owns it, the same as everything built under it
    // in this function.
    QWidget *content = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(content);

    // Row 0 is a GUARANTEED exit, independent of N3Dialog's own
    // backTapped() signal wired further below -- review finding I-3:
    // getDialog wires the dialog's X (closeTapped()) to
    // MainWindowController::closeActiveN3Dialogs(), which only affects the
    // CONTROLLER stack (MainWindowController+60), and pushView never
    // populates that stack -- so the X does nothing on this route (see
    // N3Dialog__disableCloseButton below, which removes it). If
    // backTapped() ALSO failed to fire for any reason, this screen would
    // have no way off it short of a power cycle, on the owner's daily-use
    // device. This row does not depend on N3Dialog's own signal at all --
    // it is wired straight to popView, below, once `dialog` exists -- so
    // it is the row most worth trusting if anything else on this screen
    // is wrong. Labelled unmistakably, placed first.
    char const * const kRowText[] = { "<< BACK -- tap to leave this screen", "Row 1", "Row 2" };
    QPushButton *exitShim = NULL; // wired to popView once `dialog` exists, after this loop
    for (unsigned i = 0; i < sizeof kRowText / sizeof kRowText[0]; i++) {
        // 132 bytes measured at TouchLabel's own construction call sites,
        // cross-checked across several (NOTES.md) -- NOT the 128
        // NickelHardcover uses for this exact class on this exact
        // firmware, which NOTES.md documents as a live 4-byte heap
        // overflow there. 256 is this project's usual over-allocation
        // margin for a Nickel object whose own size we cannot ask.
        // `calloc`, not `::operator new`: Qt will eventually `delete` this
        // widget itself (through its own, real vtable, once its parent
        // chain is torn down -- see the ownership note above), and
        // glibc's calloc/malloc and libstdc++'s default operator
        // new/delete share the same underlying allocator, so a
        // Qt-driven `delete` on calloc'd memory is safe -- the identical
        // assumption NickelHardcover's own construct_TouchLabel already
        // ships on this exact firmware family.
        void *row = calloc(1, 256);
        TouchLabel__ctor(row, content, 0);

        // Plain, linked QLabel::setText -- see this file's own header
        // comment on why this cast is safe and is not a redeclaration of
        // anything Nickel's own compiler generated for TouchLabel.
        reinterpret_cast<QLabel*>(row)->setText(QString::fromUtf8(kRowText[i]));

        // The signal-adaptor trick (NickelMenu, src/nickelmenu.cc): a
        // hidden QPushButton relays TouchLabel's own, real, old-style
        // tapped(bool) signal to a plain capturing lambda. TouchLabel's
        // metaobject (built by ITS OWN compiled constructor above, not by
        // anything declared in this file) is what makes the string-based
        // SIGNAL() connect below resolve correctly against the REAL
        // object -- this project has no moc step of its own and needs
        // none for this. The shim button is never shown and never
        // receives a tap itself, so the "a QPushButton is not a touch
        // target" problem this whole route exists to work around does
        // not apply to it.
        QPushButton *tapShim = new QPushButton(content);
        tapShim->setVisible(false);
        // Old-style string connects are NOT compile-checked -- a renamed
        // or missing Qt signal fails SILENTLY at runtime and returns
        // false. On this exact screen that failure is indistinguishable
        // from the gesture-pipeline failure this whole route exists to
        // work around (NOTES.md's "Task 8") unless it is logged loudly,
        // which is what review finding I-2 asks for -- matching
        // NickelMenu's own practice at this identical kind of connect
        // (src/nickelmenu.cc:379, :562).
        if (!QObject::connect(reinterpret_cast<QObject*>(row), SIGNAL(tapped(bool)), tapShim, SLOT(click())))
            nh_log("view: row %u: connecting TouchLabel::tapped(bool) to the shim button FAILED -- taps on this row will silently do nothing", i);

        if (i == 0) {
            // Wired to popView after `dialog` exists, below -- the lambda
            // needs to capture it and it is not built yet at this point
            // in the loop.
            exitShim = tapShim;
        } else {
            QObject::connect(tapShim, &QPushButton::clicked, [i] {
                nh_log("view: row %u tapped -- this milestone proves the tap, it does not act on it", i);
            });
        }

        layout->addWidget(reinterpret_cast<QWidget*>(row));
    }

    // `true`: confirmed harmless (nfnickel.h's own decode) -- builds an
    // inert GoToPageMenuController this screen never gives a page count
    // to act on.
    N3Dialog *dialog = N3DialogFactory__getDialog(content, true);
    if (!dialog) {
        nh_log("view: N3DialogFactory::getDialog returned null, refusing");
        return false;
    }

    // The guaranteed-exit row's real handler, wired now that `dialog`
    // exists to capture. `exitShim` is never NULL here -- the loop above
    // always sets it on i==0, unconditionally of whether the SIGNAL()
    // connect above it succeeded (a failed connect there is already
    // logged loudly, and this connect is a completely separate, new-style
    // one that does not depend on it).
    QObject::connect(exitShim, &QPushButton::clicked, [mwc, dialog] {
        nf_pop_native_view(mwc, dialog, "BACK row tapped");
    });

    // Restores the ndbCurrentView oracle (NOTES.md's "cosmetic" correction:
    // NDB::ndbCurrentView() reads MainWindowController::currentView()->
    // objectName(), and a bare, unnamed widget reads back empty -- not
    // evidence of a failed push, just an anonymous one). Set on the DIALOG,
    // not just the content widget, because pushView (below) is what makes
    // this the "current view", and it is the dialog, not the content
    // widget, that gets pushed -- see nfnickel.h's own comment on pushView.
    // Which widget currentView() actually returns for a pushView-pushed
    // screen is still a device question; this is the report's own
    // checklist item if the name does not show up.
    reinterpret_cast<QObject*>(dialog)->setObjectName(QStringLiteral("NFNativeView"));

    N3Dialog__setTitle(dialog, QStringLiteral("NickelFolders (native view)"));
    N3Dialog__enableBackButton(dialog, true);

    // Review finding I-3: the X (closeTapped(), pre-wired by getDialog to
    // MainWindowController::closeActiveN3Dialogs()) does nothing on this
    // route -- see N3Dialog__disableCloseButton's own comment (nfnickel.h)
    // for why. Remove the misleading affordance rather than leave a
    // visible button that does not work. NULL-gated and deliberately NOT
    // part of nf_native_view_resolve()'s hard gate: a firmware missing
    // just this symbol should still show the screen, with the X's
    // cosmetic problem left unfixed, not refuse to build the whole thing.
    if (N3Dialog__disableCloseButton)
        N3Dialog__disableCloseButton(dialog);
    else
        nh_log("view: N3Dialog::disableCloseButton did not resolve -- the X button will remain visible and will do nothing on this route (the BACK row and back gesture are this screen's real exits)");

    // N3Dialog's closeTapped() is already wired, by getDialog itself, to
    // MainWindowController::closeActiveN3Dialogs() (NOTES.md) -- but
    // backTapped() is NOT pre-wired to anything, which is what makes this
    // route's "back" our own responsibility. Same signal-adaptor trick as
    // the rows above, this time relaying the DIALOG's own real signal
    // (built by getDialog's own N3Dialog constructor, not by this file) to
    // a lambda that pops this exact widget off the stack. This is this
    // screen's SECOND exit, independent of the BACK row above -- see
    // nf_pop_native_view's own comment for why sharing that one function
    // matters here.
    QPushButton *backShim = new QPushButton(content);
    backShim->setVisible(false);
    if (!QObject::connect(reinterpret_cast<QObject*>(dialog), SIGNAL(backTapped()), backShim, SLOT(click())))
        nh_log("view: connecting N3Dialog::backTapped() to the shim button FAILED -- the back arrow/gesture will do nothing (the BACK row above is this screen's other, independent exit)");
    QObject::connect(backShim, &QPushButton::clicked, [mwc, dialog] {
        nf_pop_native_view(mwc, dialog, "backTapped() fired");
    });

    nh_log("view: pushing the native-dialog view");
    MainWindowController__pushView(mwc, reinterpret_cast<QWidget*>(dialog));
    return true;
}
