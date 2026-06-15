"""Pytest configuration for dftracer-utils tests."""

import os


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "valgrind: representative case exercising a native path; the Valgrind "
        "run executes only these in files that mark any.",
    )


def pytest_collection_modifyitems(config, items):
    if not os.environ.get("DFTRACER_UTILS_VALGRIND"):
        return

    by_file = {}
    for item in items:
        by_file.setdefault(item.nodeid.split("::", 1)[0], []).append(item)

    selected = []
    deselected = []
    for file_items in by_file.values():
        marked = [it for it in file_items if it.get_closest_marker("valgrind")]
        if marked:
            selected.extend(marked)
            deselected.extend(it for it in file_items if it not in marked)
        else:
            selected.extend(file_items)

    if deselected:
        config.hook.pytest_deselected(items=deselected)
        items[:] = selected
