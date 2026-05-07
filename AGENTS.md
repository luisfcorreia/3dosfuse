# AGENTS.md

## Project
FUSE3 userspace driver to mount 8-bit disk images. Implements IDEDOS filesystem with multi-partition support (partitions mounted as folders).

## Build
- `make`: Builds `build/plus3fuse` (requires `libfuse3-dev`)
- `make clean`: Removes `build/` artifacts

## Testing
- No automated test suite
- Manual test: `./tmount.sh` mounts `spectrum.img` to `./mnt` in foreground
- Unmount: `./tumount.sh`
- Mount structure: partitions appear as folders (e.g., `/mnt/Games/`, `/mnt/utils/`)
- Enable debug output: set `DEBUG 1` in `src/plus3dos.c:9`

## Architecture
- `src/main.c`: FUSE callbacks wired to `plus3dos_*` functions
- `src/plus3dos.c`: IDEDOS partition parsing, directory loading, file operations
- `include/plus3dos.h`: Structs and function declarations
- Only IDEDOS hard disk images supported (floppy support deleted per decision)
- Multi-partition support: partitions mounted as folders at FUSE root

## Conventions
- Standard C with GCC, no dependencies beyond `libfuse3`
- FUSE callbacks delegate to `plus3dos_*` functions
- No outside folder access
- Path structure: `/<partition>/<filename>`

## References
- `status.md`: Active development notes
