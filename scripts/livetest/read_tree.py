# SPDX-License-Identifier: GPL-3.0-only
# Copyright 2026 Austin Lehman
#
# read_tree.py - read Schultz back the way a screen reader does.
#
# Every other test in this project checks one layer against another. This one
# puts a real assistive technology client on the other end of a real
# accessibility bus and looks at what actually arrives.
#
# It is worth the trouble. It is what caught static text being published in
# the wrong property, which made every caption in the window silent while
# every unit test still passed.
#
# The client library is LGPL, which is why it is an instrument and not a
# dependency: nothing here is compiled into Schultz, linked with it, or
# shipped.

import sys
import time

try:
    import gi
    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi
except Exception as error:
    print("SKIP: no AT-SPI client library here (%s)" % error)
    sys.exit(77)

APP_NAME = "Schultz"

# The demo publishes about sixty nodes and a dozen labels on its first page.
# These are floors, well below that, and they are here because the check that
# every label carries text is true of a tree with no labels in it. A corpse on
# the bus, or an application read before it published, would otherwise pass
# this test by having nothing to be wrong about.
MIN_NODES = 10
MIN_LABELS = 3


def find_app(seconds=10.0):
    """The live instance, which is the one with a tree under it.

    A process killed rather than closed does not deregister, so the bus can
    hold several entries with the right name and nothing behind them. Taking
    the first match reads one of those corpses and reports an empty tree,
    which looks exactly like a toolkit that has stopped publishing. Pick the
    one that actually has children.
    """
    deadline = time.time() + seconds
    while time.time() < deadline:
        best = None
        best_count = 0
        for i in range(Atspi.get_desktop_count()):
            desktop = Atspi.get_desktop(i)
            for j in range(desktop.get_child_count()):
                try:
                    app = desktop.get_child_at_index(j)
                    if app is None or app.get_name() != APP_NAME:
                        continue
                    count = app.get_child_count()
                    if count > best_count:
                        best, best_count = app, count
                except Exception:
                    pass
        if best is not None and best_count > 0:
            return best
        time.sleep(0.25)
    return None


def walk(node, depth, rows, limit):
    if len(rows) >= limit:
        return
    try:
        name = node.get_name()
        role = node.get_role_name()
        count = node.get_child_count()
    except Exception:
        return
    rows.append((depth, role, name))
    for i in range(count):
        try:
            child = node.get_child_at_index(i)
        except Exception:
            continue
        if child is not None:
            walk(child, depth + 1, rows, limit)


def main():
    Atspi.init()
    app = find_app()
    if app is None:
        print("FAIL: no application named %r on the accessibility bus." % APP_NAME)
        return 1

    rows = []
    walk(app, 0, rows, 400)
    for depth, role, name in rows:
        print("  " * depth + role + (' "%s"' % name if name else ""))

    named = [r for r in rows if r[2]]
    labels = [r for r in rows if r[1] == "label"]
    silent = [r for r in labels if not r[2]]

    print()
    print("nodes: %d, named: %d, labels: %d" % (len(rows), len(named), len(labels)))

    if len(rows) < MIN_NODES or len(labels) < MIN_LABELS:
        print("FAIL: read %d nodes and %d labels, expected at least %d and %d."
              % (len(rows), len(labels), MIN_NODES, MIN_LABELS))
        print("      That is not the running demo. Most likely a registration")
        print("      left behind by a killed process: every one of those has")
        print("      the right name and nothing behind it.")
        return 1
    if not named:
        print("FAIL: nothing published a name. A reader would say nothing.")
        return 1
    if silent:
        print("FAIL: %d label nodes carry no text." % len(silent))
        print("      Static text belongs in the value property, not label.")
        return 1
    print("PASS: every published label carries text.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
