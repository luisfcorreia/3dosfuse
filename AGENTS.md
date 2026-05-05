# AGENTS.md

## Project
FUSE3 userspace driver to mount CP/M Plus (CP/M 3) 8-bit disk images, named `plus3dos`.

## Build
- `make` / `make all`: Build `build/plus3fuse` (requires `libfuse3-dev` system package)
- `make clean`: Remove `build/` artifacts
- `make test`: Placeholder for testing with CP/M disk images in `tests/`

## Structure
- `src/main.c`: FUSE entry point, callback wiring
- `src/plus3dos.c`: CP/M Plus filesystem parsing logic
- `include/plus3dos.h`: CP/M structs, function declarations
- `build/`: Compiled artifacts (gitignored)
- `tests/`: Sample disk images, test scripts

## Conventions
- Standard GCC only, no non-standard dependencies beyond `libfuse3`
- CP/M 3 structures match [CP/M Plus spec](https://www.seasip.info/Cpm/3plus.html)
- FUSE callbacks delegate to `plus3dos_*` functions
