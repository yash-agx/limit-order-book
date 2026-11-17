"""Turn a Nasdaq TotalView-ITCH 5.0 file into action files the C++ replay can read.

What ITCH is, and what this therefore tests
-------------------------------------------
ITCH is the feed Nasdaq publishes *after* its own matching engine has run. Every message
describes something that already happened to the order book: an order was added, cancelled,
deleted, replaced, or executed. It is not the raw order flow that arrived at the exchange.
In particular, during continuous trading an Add Order message never crosses the book - if an
incoming order had been executable, Nasdaq would have matched it and sent execution messages
instead.

So replaying this exercises the engines on **real order-book update flow** (real id patterns,
real price clustering, real add/cancel/execute mix, real book sizes) but **not on real
matching**. The matching path is barely touched. Nothing here measures how well an engine
matches.

The mapping to the engine's three actions
-----------------------------------------
    A, F  Add Order (without / with attribution)  -> Add a limit order
    D     Order Delete (whole order gone)         -> Cancel
    X     Order Cancel (part of an order gone)    -> Modify to the smaller quantity
    E     Order Executed                          -> Modify to the smaller quantity,
    C     Order Executed With Price                  or Cancel if nothing is left
    U     Order Replace (new price/size, new id)  -> Modify to the new price and quantity

Three honest caveats about that mapping:

1. The engine's modifyOrder is cancel + re-add, so it sends the order to the back of its
   price level. For U (replace) that is exactly right - a real replace loses time priority
   too. For X and E it is not: shrinking an order or partially filling it keeps its place in
   the real queue. The engine has no "reduce in place" call and this phase does not change
   the engines, so X/E give up time priority that the real book keeps. The set of resting
   orders and their quantities stays correct; only the order within one price level differs.

2. U gives the order a brand-new ITCH reference number. Instead of emitting a cancel and an
   add, the extractor keeps using the *original* id inside the engine and remembers that
   later messages about the new reference mean that same order. One ITCH message stays one
   engine action, and the resulting book is identical.

3. E and C carry a number of shares executed, not a remaining quantity, so the extractor has
   to keep its own copy of every tracked order's resting quantity to turn those deltas into
   the absolute quantity the engine's modifyOrder wants.

Messages that do not change the order book (P/Q trades against hidden orders, B broken
trades, and all the administrative types) are skipped.

Why only 09:30 - 16:00, and what the prefill is for
---------------------------------------------------
Before the opening cross the displayed book is routinely *crossed*: buy and sell orders for
the 09:30 auction pile up with bids above asks, and the exchange holds them instead of
matching them. Our engines match anything that crosses the moment it arrives, so replaying
the pre-open period would have them trade orders the real exchange was still holding, and
the book would stop resembling Nasdaq's. The measured window is therefore continuous trading
only, 09:30:00 to 16:00:00.

The extractor still tracks the book from the very start of the file, so at 09:30 it knows
every order resting at that moment. It writes those out first, as a block of plain adds, in
the order they originally arrived (which keeps time priority within each price level). That
block is the *prefill*: the benchmark builds the book from it untimed, exactly as it does
with the synthetic flows, and then measures the actions that follow. Without it the measured
run would start from an empty book and spend its first moments on a book of unrealistic size.

Streaming
---------
The file is 3.5 GB compressed and ~10 GB raw, so it is never loaded. It is read in 8 MB
blocks straight out of the gzip stream, and actions are written to per-symbol files as they
are found. The only thing held in memory is the currently-resting orders of the chosen
symbols (tens of thousands at most).

    python analysis/itch_extract.py
    python analysis/itch_extract.py --symbols AAPL,MSFT --max-messages 20000000
"""
import argparse
import gzip
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FILE = ROOT / "data" / "12302019.NASDAQ_ITCH50.gz"
DEFAULT_OUT = ROOT / "data" / "itch"

# Liquid Nasdaq-listed names, deliberately spread across price levels ($47 to $1850 in
# December 2019) because engine B's memory depends on the span of prices the book touches.
DEFAULT_SYMBOLS = "AAPL,MSFT,INTC,CSCO,AMZN"

# The measured window, as nanoseconds since midnight (ITCH timestamps are in that form).
OPEN_NS = 9 * 3600 * 10**9 + 30 * 60 * 10**9     # 09:30:00
CLOSE_NS = 16 * 3600 * 10**9                     # 16:00:00

BLOCK = 8 * 1024 * 1024        # bytes pulled out of the gzip stream at a time
FLUSH = 1 * 1024 * 1024        # per-symbol output buffered until this big

# ---- the ITCH wire format (everything big-endian, ">") ----
# Every message starts with: type(1) stock_locate(2) tracking_number(2) timestamp(6) = 11 bytes.
HEADER = 11
LOCATE = struct.Struct(">H")                 # stock_locate, at offset 1
DIRECTORY = struct.Struct(">8s")             # R: stock symbol, right after the header
ADD = struct.Struct(">QcI8sI")               # A/F: order_ref, buy_sell, shares, stock, price
EXECUTED = struct.Struct(">QI")              # E/C: order_ref, executed_shares
CANCELLED = struct.Struct(">QI")             # X:   order_ref, cancelled_shares
DELETED = struct.Struct(">Q")                # D:   order_ref
REPLACED = struct.Struct(">QQII")            # U:   original_ref, new_ref, shares, price

# ITCH prices are unsigned 4-byte integers with 4 implied decimals: $101.20 arrives as
# 1012000. The engines work in whole cents, so divide by 100. Displayed orders in stocks
# over $1 must be priced in whole cents, so this should never actually round anything;
# the script counts the exceptions rather than trusting that.
PRICE_SCALE = 100

# ---- the action file we write ----
ACTION_ADD, ACTION_CANCEL, ACTION_MODIFY = 0, 1, 2
RECORD = struct.Struct("<BBHIqQ")            # type, side, pad, quantity, price, id  = 24 bytes
MAGIC = b"ITCHACT1"
# 128-byte header. The first `prefill` records build the book untimed; the `actions` records
# after them are the measured flow.
FILE_HEADER = struct.Struct("<8sII8s8sQQQQqqQQQQQ8x")


class SymbolOutput:
    """One output file, written as the messages stream past."""

    def __init__(self, path, symbol, date):
        self.path, self.symbol, self.date = path, symbol, date
        self.handle = open(path, "wb")
        self.handle.write(bytes(FILE_HEADER.size))      # placeholder, rewritten at the end
        self.buffer = bytearray()
        self.prefill = 0            # records in the prefill block
        self.actions = 0            # records in the measured block
        self.messages = 0           # ITCH messages that produced a measured action
        self.counts = [0, 0, 0]     # measured adds, cancels, modifies
        self.live = 0               # orders resting right now
        self.peak_live = 0
        self.min_price = None
        self.max_price = None
        self.first_ns = 0
        self.last_ns = 0

    def write(self, kind, side, quantity, price, order_id):
        self.buffer += RECORD.pack(kind, side, 0, quantity, price, order_id)
        if len(self.buffer) >= FLUSH:
            self.handle.write(self.buffer)
            self.buffer.clear()

    def emit(self, kind, side, quantity, price, order_id):
        """One measured action."""
        self.write(kind, side, quantity, price, order_id)
        self.actions += 1
        self.counts[kind] += 1

    def saw_price(self, price):
        if self.min_price is None or price < self.min_price:
            self.min_price = price
        if self.max_price is None or price > self.max_price:
            self.max_price = price

    def close(self):
        self.handle.write(self.buffer)
        self.handle.seek(0)
        self.handle.write(FILE_HEADER.pack(
            MAGIC, 1, RECORD.size,
            self.symbol.encode().ljust(8), self.date.encode().ljust(8),
            self.prefill, self.actions, self.messages, self.peak_live,
            self.min_price or 0, self.max_price or 0,
            self.counts[ACTION_ADD], self.counts[ACTION_CANCEL], self.counts[ACTION_MODIFY],
            self.first_ns, self.last_ns))
        self.handle.close()


def reduce_order(state, shares, ref, live, trading):
    """Take `shares` off a resting order (an execution or a partial cancel).

    Emits a modify down to what is left, or a cancel if the order is used up.
    Returns 1 if the feed took off more than we think was resting, which would mean the
    extractor's own copy of the book had drifted.
    """
    engine_id, output, side, price, resting = state
    over = 1 if shares > resting else 0
    remaining = 0 if over else resting - shares
    if remaining == 0:
        del live[ref]
        output.live -= 1
        if trading:
            output.messages += 1
            output.emit(ACTION_CANCEL, side, 0, price, engine_id)
    else:
        state[4] = remaining
        if trading:
            output.messages += 1
            output.emit(ACTION_MODIFY, side, remaining, price, engine_id)
    return over


def start_trading(outputs, live):
    """09:30 has arrived: write out the book as it stands, as each symbol's prefill block."""
    for state in live.values():                 # insertion order = original arrival order
        engine_id, output, side, price, shares = state
        output.write(ACTION_ADD, side, shares, price, engine_id)
        output.prefill += 1
        output.saw_price(price)
    for output in outputs.values():             # count the peak over the measured window only
        output.peak_live = output.live


def extract(path, symbols, out_dir, max_messages):
    out_dir.mkdir(parents=True, exist_ok=True)
    date = path.name[:8]
    wanted = {s.encode() for s in symbols}
    outputs = {s: SymbolOutput(out_dir / f"{s}.actions", s, date) for s in symbols}

    # stock_locate -> that symbol's output. Filled in from the R (stock directory) messages
    # at the start of the file, which is what ties a locate code to a symbol.
    tracked = {}

    # Every resting order of a tracked symbol: ref -> [engine id, output, side, price, shares].
    # "ref" is the ITCH order reference currently naming the order, which U changes;
    # "engine id" is the id the engine knows it by, which never changes.
    live = {}

    total = 0                                  # every message in the file
    rounded = skipped = oversized = 0
    trading = False                            # has 09:30 been reached?
    stop = False

    with gzip.open(path, "rb") as source:
        data = source.read(BLOCK)
        at = 0
        while not stop:
            # Refill when the next length prefix, or the message it announces, is cut off.
            if len(data) - at < 2 or len(data) - at - 2 < ((data[at] << 8) | data[at + 1]):
                more = source.read(BLOCK)
                if not more:
                    break
                data = data[at:] + more
                at = 0
                if len(data) < 2:
                    break
            length = (data[at] << 8) | data[at + 1]
            if length == 0:
                break                          # end-of-session filler
            base = at + 2
            at = base + length
            total += 1
            if max_messages and total >= max_messages:
                stop = True
            kind = data[base]

            # The timestamp is only unpacked for messages that concern a tracked symbol -
            # doing it for all ~230 million would dominate the run time. Liquid names send
            # messages constantly, so the window boundaries are still found to the microsecond.
            if kind == 0x41 or kind == 0x46:           # 'A' / 'F' add order
                output = tracked.get(LOCATE.unpack_from(data, base + 1)[0])
                if output is None:
                    continue
                stamp = int.from_bytes(data[base + 5:base + HEADER], "big")
                if not trading and stamp >= OPEN_NS:
                    start_trading(outputs, live)
                    trading = True
                if stamp >= CLOSE_NS:
                    break
                ref, buy_sell, shares, _stock, raw = ADD.unpack_from(data, base + HEADER)
                if raw % PRICE_SCALE:
                    rounded += 1
                price = (raw + PRICE_SCALE // 2) // PRICE_SCALE
                if price <= 0 or shares == 0:           # not a usable displayed limit order
                    skipped += 1
                    continue
                side = 0 if buy_sell == b"B" else 1
                live[ref] = [ref, output, side, price, shares]
                output.live += 1
                if output.live > output.peak_live:
                    output.peak_live = output.live
                if trading:
                    output.messages += 1
                    output.last_ns = stamp
                    if output.first_ns == 0:
                        output.first_ns = stamp
                    output.saw_price(price)
                    output.emit(ACTION_ADD, side, shares, price, ref)

            elif kind == 0x45 or kind == 0x43:         # 'E' / 'C' executed: shares leave the book
                ref, shares = EXECUTED.unpack_from(data, base + HEADER)
                state = live.get(ref)
                if state is not None:
                    oversized += reduce_order(state, shares, ref, live, trading)

            elif kind == 0x58:                         # 'X' part of an order cancelled
                ref, shares = CANCELLED.unpack_from(data, base + HEADER)
                state = live.get(ref)
                if state is not None:
                    oversized += reduce_order(state, shares, ref, live, trading)

            elif kind == 0x44:                         # 'D' the whole order is gone
                ref = DELETED.unpack_from(data, base + HEADER)[0]
                state = live.pop(ref, None)
                if state is not None:
                    engine_id, output, side, price, _shares = state
                    output.live -= 1
                    if trading:
                        output.messages += 1
                        output.emit(ACTION_CANCEL, side, 0, price, engine_id)

            elif kind == 0x55:                         # 'U' replace: new price, size and ref
                ref, new_ref, shares, raw = REPLACED.unpack_from(data, base + HEADER)
                state = live.pop(ref, None)
                if state is None:
                    continue
                engine_id, output, side, old_price, _shares = state
                if raw % PRICE_SCALE:
                    rounded += 1
                price = (raw + PRICE_SCALE // 2) // PRICE_SCALE
                if price <= 0 or shares == 0:           # replaced into something undisplayable
                    output.live -= 1
                    if trading:
                        output.messages += 1
                        output.emit(ACTION_CANCEL, side, 0, old_price, engine_id)
                    continue
                # Keep the engine id; later messages will arrive under new_ref.
                live[new_ref] = [engine_id, output, side, price, shares]
                if trading:
                    output.messages += 1
                    output.saw_price(price)
                    output.emit(ACTION_MODIFY, side, shares, price, engine_id)

            elif kind == 0x52:                         # 'R' stock directory: locate -> symbol
                stock = DIRECTORY.unpack_from(data, base + HEADER)[0].strip()
                if stock in wanted:
                    tracked[LOCATE.unpack_from(data, base + 1)[0]] = outputs[stock.decode()]

    for output in outputs.values():
        output.close()
    return outputs, total, rounded, skipped, oversized


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--file", type=Path, default=DEFAULT_FILE)
    parser.add_argument("--symbols", default=DEFAULT_SYMBOLS)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--max-messages", type=int, default=0, help="stop early (0 = whole file)")
    args = parser.parse_args()

    if not args.file.exists():
        raise SystemExit(f"{args.file} not found - run: python analysis/itch_download.py")
    symbols = [s.strip().upper() for s in args.symbols.split(",") if s.strip()]

    print(f"Streaming {args.file.name} for {', '.join(symbols)}")
    outputs, total, rounded, skipped, oversized = extract(args.file, symbols, args.out, args.max_messages)

    print(f"\n{total:,} ITCH messages read")
    if rounded:
        print(f"WARNING: {rounded:,} prices were not a whole cent and were rounded")
    if skipped:
        print(f"{skipped:,} add messages had no usable price or zero shares and were skipped")
    if oversized:
        print(f"WARNING: {oversized:,} executions/cancels exceeded the quantity we had resting")

    print(f"\n{'symbol':<7}{'messages':>12}{'prefill':>10}{'actions':>12}{'adds':>12}"
          f"{'cancels':>12}{'modifies':>12}{'peak live':>11}{'price range ($)':>20}")
    for symbol, output in outputs.items():
        span = f"{(output.min_price or 0) / 100:.2f}-{(output.max_price or 0) / 100:.2f}"
        print(f"{symbol:<7}{output.messages:>12,}{output.prefill:>10,}{output.actions:>12,}"
              f"{output.counts[0]:>12,}{output.counts[1]:>12,}{output.counts[2]:>12,}"
              f"{output.peak_live:>11,}{span:>20}")
    print(f"\nAction files written to {args.out}")


if __name__ == "__main__":
    main()
