"""Download a Nasdaq TotalView-ITCH 5.0 sample file into data/.

Nasdaq serves these over one connection at only ~200 KB/s, which would take hours for a
3.5 GB file, but the server supports HTTP range requests. So this asks for the file in
32 MB pieces over several connections at once and writes each piece straight into its own
place in the output file. That gets ~20x the speed.

It is resumable: finished pieces are recorded in a .progress file next to the download, so
re-running after a dropped connection only fetches what is missing.

    python analysis/itch_download.py                  # the default file (12302019, 3.5 GB)
    python analysis/itch_download.py 01302019         # a different trading day

The gz is left compressed - itch_extract.py streams it without unpacking it, and nothing
here ever holds the whole file in memory. data/ is gitignored: the file is far too big for git.
"""
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

BASE = "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/"
DATA = Path(__file__).resolve().parent.parent / "data"

CHUNK = 32 * 1024 * 1024   # bytes per range request
WORKERS = 8                # parallel connections
ATTEMPTS = 5               # retries per chunk before giving up


def size_of(url):
    request = urllib.request.Request(url, method="HEAD")
    with urllib.request.urlopen(request, timeout=60) as response:
        return int(response.headers["Content-Length"])


def fetch(url, path, index, lo, hi):
    """Download bytes [lo, hi] and write them at offset lo. Returns the chunk index."""
    for attempt in range(ATTEMPTS):
        try:
            request = urllib.request.Request(url, headers={"Range": f"bytes={lo}-{hi}"})
            with urllib.request.urlopen(request, timeout=120) as response:
                data = response.read()
            if len(data) != hi - lo + 1:
                raise OSError(f"got {len(data)} bytes, wanted {hi - lo + 1}")
            # Each thread opens its own handle and seeks to its own slice, so the writes
            # never overlap and no locking is needed.
            with open(path, "r+b") as out:
                out.seek(lo)
                out.write(data)
            return index
        except Exception as error:                       # noqa: BLE001 - any network error is worth a retry
            if attempt == ATTEMPTS - 1:
                raise
            print(f"  chunk {index} failed ({error}), retrying", flush=True)
    return index


def main():
    day = sys.argv[1] if len(sys.argv) > 1 else "12302019"
    name = f"{day}.NASDAQ_ITCH50.gz"
    url, path = BASE + name, DATA / name
    progress = path.with_suffix(path.suffix + ".progress")

    DATA.mkdir(exist_ok=True)
    total = size_of(url)
    chunks = [(i, lo, min(lo + CHUNK, total) - 1) for i, lo in enumerate(range(0, total, CHUNK))]

    # Make the file its final length up front so any chunk can be written at its own offset.
    if not path.exists() or path.stat().st_size != total:
        with open(path, "wb") as out:
            out.truncate(total)
        progress.unlink(missing_ok=True)

    done = set()
    if progress.exists():
        done = {int(line) for line in progress.read_text().split()}
    todo = [c for c in chunks if c[0] not in done]
    print(f"{name}: {total / 1e9:.2f} GB, {len(chunks)} chunks, {len(todo)} still to fetch")

    # as_completed, not the submission order: a chunk is recorded the moment it lands, so a
    # stalled chunk cannot hide the progress of the others and a resume refetches only what
    # is genuinely missing.
    with open(progress, "a") as log, ThreadPoolExecutor(WORKERS) as pool:
        futures = [pool.submit(fetch, url, path, *c) for c in todo]
        for finished, future in enumerate(as_completed(futures), 1):
            log.write(f"{future.result()}\n")
            log.flush()
            print(f"  {finished}/{len(todo)} chunks ({finished * CHUNK / 1e9:.2f} GB)", flush=True)

    progress.unlink(missing_ok=True)
    print(f"Saved {path}")


if __name__ == "__main__":
    main()
