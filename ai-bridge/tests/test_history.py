"""bridge.history has no pure helpers to unit-test.

Every meaningful piece of logic (_sample's state-diffing, _loot's dedup/count,
_skillups, _boss_firsts) lives inside async HistoryRecorder methods that take a
live aiomysql cursor as an argument and interleave SQL execution with the
decision logic in the same method body - there is no extracted pure function
(parsing/formatting/trim) callable without a DB pool. Exercising them would mean
faking an async cursor end-to-end, which is exactly the "no DB" line this suite
is told to stay behind, and refactoring src/ to separate the logic is out of
scope. Skipped per the test brief's instruction for this module.
"""

import pytest

pytest.skip(
    "no pure helpers in bridge.history: all logic is embedded in async DB-cursor "
    "methods (_sample/_loot/_skillups/_boss_firsts); would require either a fake "
    "DB pool or refactoring src/, both out of scope",
    allow_module_level=True,
)
