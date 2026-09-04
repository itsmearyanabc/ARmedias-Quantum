"""Data ingest for the XAUUSD trading terminal.

Everything here is I/O bound or one-off, which is why it stays in Python — see
docs/PLAN.md section 3 for what belongs on which side of the pybind11 boundary.

    tickfmt    the binary tick format: writer, reader, verifier
    synth      synthetic tick generator (test data, and later the
               random-walk null used by the leakage tests)
    dukascopy  historical tick fetch and decode
    mt5spec    MT5 symbol spec + server clock offset discovery
"""

__all__ = ["tickfmt", "synth", "dukascopy", "mt5spec"]
__version__ = "0.1.0"
