"""Run the corpus scripts' per-track work concurrently, in corpus order.

Each track is independent (its own decode, harness runs and scores), so the tracks run in a
process pool and the results come back in the order of the track list: the output and the
gate's numbers are the same as one track at a time.
"""
from __future__ import annotations

import os
from collections.abc import Callable, Iterable, Iterator
from concurrent.futures import ProcessPoolExecutor
from typing import TypeVar

T = TypeVar("T")
R = TypeVar("R")

DEFAULT_JOBS = os.cpu_count() or 1


def map_tracks(fn: Callable[[T], R], stems: Iterable[T], jobs: int = DEFAULT_JOBS) -> Iterator[R]:
    """fn over the tracks, `jobs` processes at a time; results in the tracks' order.

    Flush stdout before calling: a forked worker flushes what it inherited when it exits.
    """
    stems = list(stems)
    if jobs <= 1 or len(stems) <= 1:
        yield from map(fn, stems)
        return
    with ProcessPoolExecutor(max_workers=min(jobs, len(stems))) as pool:
        yield from pool.map(fn, stems)
