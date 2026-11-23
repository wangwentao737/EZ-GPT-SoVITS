"""Measure the actual 60-row history queries using a 10,000-job portable index."""

import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import time
import uuid


SELECT = "SELECT id,created,operation,state,message,result,request,rowid FROM jobs"
FIRST = SELECT + " ORDER BY created DESC,rowid DESC LIMIT 61"
NEXT = SELECT + " WHERE (created,rowid)< (?,?) ORDER BY created DESC,rowid DESC LIMIT 61"
BATCH = (SELECT + " WHERE json_valid(CAST(request AS TEXT))"
         " AND json_extract(CAST(request AS TEXT),'$.batch_id')=? AND rowid>? ORDER BY rowid LIMIT 61")


def timed(db, sql, values=()):
    start = time.perf_counter()
    rows = db.execute(sql, values).fetchall()
    return rows, (time.perf_counter() - start) * 1000


def main():
    workspace = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    args = parser.parse_args()
    program = args.app.resolve(strict=True)
    data = workspace / ".test" / f"history-{uuid.uuid4().hex}"
    invocation = [str(program), "--data-root", str(data), "--smoke-test"]
    options = {"env": {**os.environ, "QT_QPA_PLATFORM": "windows"},
               "creationflags": subprocess.CREATE_NO_WINDOW, "check": True, "timeout": 30}
    subprocess.run(invocation, **options)
    with closing(sqlite3.connect(data / "library.sqlite")) as db, db:
        db.executemany("INSERT INTO jobs(id,created,launcher,operation,state,message,result,request)"
                       "VALUES(?,?,?,?,?,?,?,?)",
            ((str(uuid.uuid4()), f"2026-11-23T00:{index // 200:02}:00.000Z", "fixture", "synthesize",
              "succeeded", "", b"{}", json.dumps({"text": f"Line {index}", "batch_id": str(index // 200)}).encode())
             for index in range(10000)))
        first, first_ms = timed(db, FIRST)
        anchor = first[59]
        second, next_ms = timed(db, NEXT, (anchor[1], anchor[-1]))
        assert len(first) == len(second) == 61
        assert set(row[0] for row in first[:60]).isdisjoint(row[0] for row in second)
        batch, batch_ms = timed(db, BATCH, ("49", 0))
        assert len(batch) == 61
        assert db.execute("SELECT COUNT(*) FROM jobs WHERE json_valid(CAST(request AS TEXT)) AND "
                          "json_extract(CAST(request AS TEXT),'$.batch_id')='49'").fetchone() == (200,)
        db.execute("UPDATE jobs SET state='failed' WHERE id=?", (second[7][0],))
        refreshed, refresh_ms = timed(db, NEXT, (anchor[1], anchor[-1]))
        assert [row[0] for row in second] == [row[0] for row in refreshed]
        assert sum(before != after for before, after in zip(second, refreshed, strict=True)) == 1
        for sql, params, expected in ((FIRST, (), "jobs_created"), (NEXT, (anchor[1], anchor[-1]), "jobs_created"),
                                      (BATCH, ("49", 0), "jobs_batch")):
            plan = db.execute("EXPLAIN QUERY PLAN " + sql, params).fetchall()
            assert any(expected in row[-1] for row in plan), plan
    subprocess.run(invocation, **options)
    print(f"10,000 jobs. SQLite first {first_ms:.2f} ms, next {next_ms:.2f} ms, "
          f"batch {batch_ms:.2f} ms, refresh one changed row {refresh_ms:.2f} ms.")


if __name__ == "__main__":
    main()
