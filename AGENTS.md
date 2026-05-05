# AGENTS.md

## Project
FUSE3 userspace driver to mount 8-bit disk images, named `plus3dos`.

## Build
- `make` / `make all`: Build `build/plus3fuse` (requires `libfuse3-dev` system package)
- `make clean`: Remove `build/` artifacts
- `make test`: Placeholder for testing with disk images in `tests/`
- `images should be mounted locally in ./mnt`

## Structure
- `src/main.c`: FUSE entry point, callback wiring
- `src/plus3dos.c`: CP/M Plus filesystem parsing logic
- `include/plus3dos.h`: CP/M structs, function declarations
- `build/`: Compiled artifacts (gitignored)
- `tests/`: Sample disk images, test scripts

## Conventions
- Standard GCC only, no non-standard dependencies beyond `libfuse3`
- FUSE callbacks delegate to `plus3dos_*` functions
