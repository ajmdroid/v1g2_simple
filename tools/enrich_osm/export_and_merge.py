#!/usr/bin/env python3
"""Export enriched cameras from PostGIS and merge back into NDJSON."""

import argparse
import json
from pathlib import Path

import psycopg2


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--table", default="cameras_enriched", help="Enriched table name")
    p.add_argument("--output", required=True, type=Path, help="Output NDJSON path")
    p.add_argument(
        "--dsn", default="postgresql://osm:osm@localhost:5432/osm", help="PostgreSQL DSN"
    )
    return p.parse_args()


def main():
    args = parse_args()
    conn = psycopg2.connect(args.dsn)

    with conn.cursor() as cur, args.output.open("w", encoding="utf-8") as out:
        cur.itersize = 10000
        cur.execute(
            f"SELECT enriched FROM {args.table} ORDER BY id;"
        )
        count = 0
        for row in cur:
            out.write(json.dumps(row[0]) + "\n")
            count += 1
    conn.close()
    print(f"Wrote {count} records to {args.output}")


if __name__ == "__main__":
    main()
