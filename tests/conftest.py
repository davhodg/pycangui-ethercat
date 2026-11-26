# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 davhodg
"""One offscreen QApplication for the whole test session, and a drain between
tests -- pycangui's own test setup, as the plugin is tested inside pycangui.

The same as pycangui-cia402's, without the demo device: queued signals from
worker threads are delivered while their targets are alive, windows a test made
are destroyed after it, and a modal dialog nobody answers fails the test rather
than hanging it.
"""

import gc
import os
import sys
import weakref
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

#: The repository, so that the plugin imports as ``ethercat`` in the tests.
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

# python-can is loaded the way the application loads it, without the MF4
# support it would otherwise pull in. Otherwise a release of asammdf, or of
# something asammdf uses, that will not import takes ``import can`` with it
# and every test in the suite fails to be collected -- which is what a bad
# canmatrix release did to CI -- where only the tests that read MDF should.
from pycangui.__main__ import import_can_without_mf4  # noqa: E402

import_can_without_mf4()

from PySide6.QtCore import QCoreApplication, QEvent, QSettings  # noqa: E402
from PySide6.QtWidgets import QApplication, QDialog, QMessageBox  # noqa: E402

#: Taken before anything patches it, so a test that wants a real dialog can
#: have one back.
_REAL_EXEC = {QDialog: QDialog.exec, QMessageBox: QMessageBox.exec}


@pytest.fixture(scope="session", autouse=True)
def _isolate_settings(tmp_path_factory):
    """Keep QSettings in a temporary file rather than the registry.

    MainWindow saves its geometry and dock layout in closeEvent, so without
    this a test run leaves settings behind, and the next run restores a layout
    from the last one -- which is exactly what a layout test must not see.
    """
    QSettings.setDefaultFormat(QSettings.IniFormat)
    QSettings.setPath(QSettings.IniFormat, QSettings.UserScope, str(tmp_path_factory.mktemp("settings")))


@pytest.fixture(scope="session")
def app():
    return QApplication.instance() or QApplication([])


# --- a window a test made goes when the test does ------------------------------
#: The windows there were before the test now running began.
_WINDOWS_BEFORE: set = set()


def pytest_runtest_setup(item):
    instance = QApplication.instance()
    _WINDOWS_BEFORE.clear()
    if instance is not None:
        _WINDOWS_BEFORE.update(instance.topLevelWidgets())


def _ours_to_destroy(widget) -> bool:
    """A window of pycangui's own, with nothing above it that will take it away.

    Not one that has a parent: a drop-down's list and a button's menu are
    windows as far as Qt is concerned, and belong to the widget that made
    them, which deletes them itself -- destroyed here first, that is the
    same object deleted twice. And not one of Qt's own classes with no
    parent: those found here are pyqtgraph's, a plot's control panel and
    its menus, which the plot's Python side owns, and destroying them under
    it is a fault when the process exits. What is left is what a test
    makes and means: a main window, a detached pane, a dialog.
    """
    return widget.parent() is None and not type(widget).__module__.startswith("PySide6")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_teardown(item):
    """Destroy the windows the test made, once its fixtures have finished with them.

    Closing a window hides it and no more. Letting go of it does not free it
    either: its signals are connected to functions that hold the window, Qt
    holds the connections, and that is a ring the garbage collector cannot
    see round. So every window every test made stayed for the rest of the
    run -- 1,200 widgets each, 200,000 in a process by the end -- and each
    collection, and anything said to every widget, got slower as they piled
    up. Destroying the window takes its connections with it, and the rest
    then goes the ordinary way.

    After the fixtures and not in one: a fixture that closes its window on
    the way out needs the window to be there. A fixture wider than one test
    must not make a window, since the first test to use it would take it away.
    """
    yield
    instance = QApplication.instance()
    if instance is None:
        return
    for window in instance.topLevelWidgets():
        if window in _WINDOWS_BEFORE or not _ours_to_destroy(window):
            continue
        if getattr(window, "_closing", None) is False:
            # A main window its test never closed. Closing is what stops its
            # threads, and Qt ends the process over a thread destroyed while
            # it runs. Said to be closing already, so that it does not stop
            # to ask about anything unsaved: nobody is there to answer.
            window._closing = True
            window.close()
        window.deleteLater()
    _WINDOWS_BEFORE.clear()
    # Asked for by name: with no event loop running, nothing else delivers them.
    QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)


@pytest.fixture(autouse=True)
def _no_unanswered_dialogs(monkeypatch):
    """A modal dialog nobody answered blocks forever.

    Offscreen or not, ``exec`` runs its own event loop and waits, so a test
    that reaches an unexpected dialog does not fail -- it hangs, which is the
    worst thing in a suite to work out. This turns that into an ordinary
    failure naming the dialog. A test that means to answer one patches
    ``exec`` itself, and its patch replaces this one.
    """

    def refuse(self, *_args, **_kwargs):
        raise AssertionError(
            f"a modal {type(self).__name__} opened that no test answered: {self.windowTitle()!r}"
        )

    for widget in (QDialog, QMessageBox):
        monkeypatch.setattr(widget, "exec", refuse)


@pytest.fixture
def real_dialogs(monkeypatch):
    """Put ``exec`` back, for the few tests that mean to open a real dialog.

    A test checking what happens *while* one is up has to have a real one, and
    arranges to close it itself.
    """
    for widget in (QDialog, QMessageBox):
        monkeypatch.setattr(widget, "exec", _REAL_EXEC[widget])


#: Background threads a test started, kept weakly so registering one here
#: never keeps it alive. See ``_drain_qt_events`` for what is done with them.
_THREADS: weakref.WeakSet = weakref.WeakSet()


def _still_going(thread) -> bool:
    """Whether this thread is running, asked in whichever way it answers.

    A ``QThread`` has ``isRunning``; a python-can ``Notifier`` has ``stopped``.
    Stopping one that has already stopped is not harmless: ``Worker.stop``
    disconnects a signal, and PySide warns loudly when there is nothing left
    to disconnect, which would fill the run with warnings about tests that
    tidied up properly.
    """
    try:
        if (running := getattr(thread, "isRunning", None)) is not None:
            return bool(running())
        return not getattr(thread, "stopped", False)
    except RuntimeError:  # the C++ side has gone, so nothing is running
        return False


def _watch(cls, monkeypatch):
    """Note every instance of ``cls`` a test makes, for stopping afterwards."""
    original = cls.__init__

    def made(self, *args, **kwargs):
        original(self, *args, **kwargs)
        _THREADS.add(self)

    monkeypatch.setattr(cls, "__init__", made)


@pytest.fixture(autouse=True)
def _drain_qt_events(monkeypatch):
    import can
    from pycangui.core.logging import Player
    from pycangui.core.worker import Worker

    for cls in (Player, Worker, can.Notifier):
        _watch(cls, monkeypatch)
    yield
    # Nothing a test started may outlive it. A QThread still running when Qt
    # destroys its C++ side aborts the process -- Worker.submit says the same
    # thing from the other end -- and a replay player or a python-can Notifier
    # left running sends frames into the next test's bus. Stopping them here
    # rather than leaving it to each test means a test that forgets is slow,
    # not a crash in whatever ran afterwards; that is how a replay player
    # outliving its test killed a CI worker in test_export and test_right_axis.
    for thread in list(_THREADS):
        if not _still_going(thread):
            continue  # its owner stopped it, which is the normal case
        try:
            thread.stop()
        except Exception:  # half-built, or stopping twice
            pass
        if (wait := getattr(thread, "wait", None)) is not None:
            wait(2000)
    _THREADS.clear()
    # Fixtures tear down after this point, so deliver what is already queued
    # first, then let go of anything unreferenced before the next test.
    instance = QCoreApplication.instance()
    if instance is not None:
        for _ in range(3):
            instance.processEvents()
    # Everything, not only the young generations. Collecting generations 0
    # and 1 is much cheaper -- a full collect walks the session's QApplication
    # and every imported module, and cost 78s against 48s for the suite -- but
    # it leaves anything that survived two collections mid-test to Python's
    # own sweep, at an arbitrary later moment rather than here, where the
    # queue has just been drained and no background thread is running. Qt
    # objects freed at that arbitrary moment are what CI kept dying on.
    gc.collect()
    if instance is not None:
        instance.processEvents()
