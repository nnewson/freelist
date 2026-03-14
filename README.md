# Free List in C++

Setup [vcpkg](https://vcpkg.io/en/) on the build machine, and ensure that VCPKG_ROOT is available in the PATH environment variable.
Details of how to do this can be found at steps 1 and 2 in this [getting started doc](https://learn.microsoft.com/en-gb/vcpkg/get_started/get-started).
Ensure the `vcpkg` executable is available in your `PATH`.

Configure CMake, which will install and build dependencies via vcpkg.
Additionally, since I use `NeoVim`, I export the `compile_commands.json` to the build directory to for use with `clangd`:

```bash
cmake --preset=vcpkg -DCMAKE_EXPORT_COMPILE_COMMANDS=1
```

Build the test via CMake:

```bash
cmake --build build
```

Run the tests:

```bash
./build/test_wait_free_queue
```

Build documentation into the docs folder:

```bash
doxygen Doxyfile
```

You can also build and test in a Docker containter.

Build the container:

```bash
docker build -t freelist-test .
```

Run the tests in the container:

```bash
docker run --rm freelist-test

```

---

## Why?

The project was put together to allow a client to access a free list (I use it with 
my Wait-Free queue).  The Free List itself is Lock Free though, not Wait Free.

It allows the client to use the Free List in single threaded or multi-threaded modes,
and with different types performing the allocate or free.

An example being have multiple threads allocate from the Free List, and then having a
single consumer free that node once it's processed it.

## Platform Requirements

This library requires a **64-bit platform** (`sizeof(void*) == 8`).

### Tagged Pointer (ABA Prevention)

The multi-threaded construct path (`FreeListMTConstruct`) uses a lock-free
compare-and-swap (CAS) loop to pop nodes from the free list. To prevent the
[ABA problem](https://en.wikipedia.org/wiki/ABA_problem), the head pointer is
paired with a 16-bit generation counter packed into the upper bits of a single
pointer-sized word (`TaggedPtr`). This keeps `std::atomic<TaggedPtr>` at 8
bytes, guaranteeing lock-free atomics on all mainstream 64-bit platforms without
requiring 128-bit CAS support.

The packing relies on the fact that current 64-bit platforms do not use the
upper 16 bits of a virtual address for user-space pointers:

| Platform | Virtual Address Bits | Upper Bits Available |
| --- | --- | --- |
| x86_64 Linux (default) | 48 | 16 |
| x86_64 Windows | 48 | 16 |
| ARM64 Linux (default) | 48 | 16 |
| ARM64 macOS | 47 | 17 |

A debug assertion in the `TaggedPtr` constructor validates at runtime that no
allocated pointer uses the upper bits, catching misuse immediately in testing.

**Known incompatible configurations:**

- **x86_64 with 5-level paging (LA57):** Uses 57-bit virtual addresses, leaving
  only 7 upper bits. This is an opt-in kernel feature (`CONFIG_X86_5LEVEL`) and
  user-space allocators will not return 57-bit addresses unless explicitly
  requested via `mmap` flags. If you need LA57 support, the `TaggedPtr` tag
  width must be reduced or an alternative ABA prevention strategy used.
- **ARM64 with Pointer Authentication (PAC):** The hardware stores
  authentication codes in the upper pointer bits. Packing a tag over them will
  cause authentication traps.
- **ARM64 with Memory Tagging Extension (MTE):** Uses bits 59:56 for memory
  tags, which overlap with the packed generation counter.

For the vast majority of deployments on standard Linux, macOS, and Windows these
restrictions do not apply.
