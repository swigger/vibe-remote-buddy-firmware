#!/usr/bin/env python3
"""Compatibility entry point; arguments and stdin are unchanged."""
try:
    from vibeled_agent_hook import entry
    entry()
except BaseException:
    raise SystemExit(0)
