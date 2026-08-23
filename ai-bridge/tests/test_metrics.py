"""Pure-logic tests for bridge.metrics: _escape() and _emit() text formatting.

Deliberately does not touch prometheus_text()/_rows() - those require a live
aiomysql pool, which is out of scope per the test brief.
"""

from __future__ import annotations

from bridge.metrics import _emit, _escape


def test_escape_backslash_quote_and_newline_individually() -> None:
    assert _escape("a\\b") == "a\\\\b"
    assert _escape('she said "hi"') == 'she said \\"hi\\"'
    assert _escape("line1\nline2") == "line1\\nline2"


def test_escape_combined_case_matches_sequential_replace_order() -> None:
    # Order matters: backslash is escaped first, so the backslash introduced by
    # escaping the quote/newline must not itself be re-escaped.
    assert _escape('a\\b"c\nd') == 'a\\\\b\\"c\\nd'


def test_emit_unlabelled_series_writes_help_type_and_bare_value() -> None:
    out: list[str] = []
    _emit(
        out,
        "aetherion_bots_online",
        "gauge",
        "Characters currently online.",
        [(None, 42)],
    )
    assert out == [
        "# HELP aetherion_bots_online Characters currently online.",
        "# TYPE aetherion_bots_online gauge",
        "aetherion_bots_online 42",
    ]


def test_emit_labelled_series_renders_one_line_per_sample_in_order() -> None:
    out: list[str] = []
    _emit(
        out,
        "aetherion_needs",
        "gauge",
        "Open needs, by type.",
        [({"type": "mount"}, 3), ({"type": "epic gear"}, 0)],
    )
    assert out[2:] == [
        'aetherion_needs{type="mount"} 3',
        'aetherion_needs{type="epic gear"} 0',
    ]


def test_emit_escapes_label_values_via_escape() -> None:
    out: list[str] = []
    _emit(
        out,
        "aetherion_econ_events_total",
        "counter",
        "help",
        [({"kind": 'weird"kind'}, 1)],
    )
    assert out[2] == 'aetherion_econ_events_total{kind="weird\\"kind"} 1'


def test_emit_appends_rather_than_overwrites_existing_output() -> None:
    out = ["# a pre-existing line"]
    _emit(out, "m", "gauge", "h", [(None, 1)])
    assert out[0] == "# a pre-existing line"
    assert len(out) == 4
