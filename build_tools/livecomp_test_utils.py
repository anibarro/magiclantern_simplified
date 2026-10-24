"""Extract real C functions for host-side module contract tests."""
import re

def function(source, name):
    match = re.search(r"^static [^\n]*\b" + re.escape(name), source, re.M)
    start = match.start()
    depth = 0
    for end in range(source.index("{", match.end()), len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end+1]
    raise ValueError(name)
