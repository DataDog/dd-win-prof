#!/usr/bin/env python3
"""Create an isolated, test-only Cargo harness against an unmodified v19 source tree."""
import argparse
from pathlib import Path
import shutil
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--libdatadog-source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
source = a.libdatadog_source.resolve()
if not (source / 'datadog-profiling/Cargo.toml').is_file():
    p.error('source must be a libdatadog v19 repository/archive root')
if a.output.exists():
    p.error('output must not exist; do not overwrite another harness')
here = Path(__file__).resolve().parent / 'baseline'
shutil.copytree(here, a.output)
manifest = a.output / 'Cargo.toml'
manifest.write_text(manifest.read_text().replace('LIBDATADOG_SOURCE', source.as_posix()))
print(f'Prepared test-only v19 baseline: {a.output}')
