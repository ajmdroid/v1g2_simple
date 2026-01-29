#!/usr/bin/env python3
"""Load camera NDJSON into PostGIS staging table.

- Reads NDJSON (one JSON object per line) with lat/lon keys.
- Writes to a staging table with geometry.
- Designed for repeatable runs (table is dropped and recreated unless --append).
"""

import argparse
import json
import sys
from pathlib import Path

import psycopg2
from psycopg2.extras import execute_values


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path, help="Input NDJSON camera file")
    p.add_argument("--table", default="cameras_stage", help="Staging table name")
    p.add_argument("--append", action="store_true", help="Append instead of recreate table")
    p.add_argument("--limit", type=int, default=None, help="Optional row limit for testing")
    p.add_argument(
        "--dsn",
        default="postgresql://osm:osm@localhost:5432/osm",
        help="PostgreSQL DSN",
    )
    return p.parse_args()


def ensure_table(conn, table: str, append: bool):
    with conn.cursor() as cur:
        if not append:
            cur.execute(f"DROP TABLE IF EXISTS {table};")
        cur.execute(
            f"""
            CREATE TABLE IF NOT EXISTS {table} (
                id BIGSERIAL PRIMARY KEY,
                src jsonb NOT NULL,
                geom geometry(Point, 4326) NOT NULL
            );
            """
        )
        cur.execute(f"CREATE INDEX IF NOT EXISTS idx_{table}_geom ON {table} USING GIST(geom);")
    conn.commit()


def load_rows(conn, table: str, rows):
    with conn.cursor() as cur:
        execute_values(
            cur,
            f"INSERT INTO {table} (src, geom) VALUES %s",
            rows,
            page_size=5000,
        )
    conn.commit()


def main():
    args = parse_args()
    conn = psycopg2.connect(args.dsn)
    ensure_table(conn, args.table, args.append)

    batch = []
    processed = 0
    with args.input.open("r", encoding="utf-8") as fh:
        for line in fh:
            if args.limit and processed >= args.limit:
                break
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            lat = obj.get("lat")
            lon = obj.get("lon")
            if lat is None or lon is None:
                continue
            batch.append((json.dumps(obj), f"SRID=4326;POINT({lon} {lat})"))
            processed += 1
            if len(batch) >= 5000:
                load_rows(conn, args.table, batch)
                batch.clear()
    if batch:
        load_rows(conn, args.table, batch)

    print(f"Loaded {processed} rows into {args.table}")
    conn.close()


if __name__ == "__main__":
    main()
