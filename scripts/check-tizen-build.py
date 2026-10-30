#!/usr/bin/env python3
"""Checks on the WASM build that the TV would otherwise catch at run time.

1. ihslib's archive must not contain the backends this project replaces. Emscripten's
   toolchain used to update archives in place, so the object of a removed source stayed in
   libihslib.a and won at link time: ihslib's own UDP backend calls fcntl(F_SETFL) on its
   socket, which Samsung's socket layer answers with abort().
2. The module must not import open (___syscall5). File syscalls from worker threads are
   proxied, synchronously, to the browser main thread; mbedTLS used to read /dev/urandom
   that way for every encrypted control message.

3. Every C function handed to ihslib as a thread or timer callback must take the number of
   parameters its callback type declares. Emscripten fastcomp keeps one function table per
   signature, so a function called through a pointer of another signature is looked up in
   the wrong table and some unrelated function runs instead. ihslib's timer thread worker
   was declared without parameters, and on the TV the timer thread never ran a task.
   ihslib builds with -w, so the C compiler says nothing about these.
4. The module must not use 64-bit atomic instructions. Tizen 5.5's WebAssembly compiler hangs
   on them before main() runs, and std::atomic<uint64_t> makes them; 64-bit counters use
   Counter64 (src/core/counter.h) instead.

The module does import fcntl (___syscall221), from musl's socket() fallback, which only
runs for SOCK_NONBLOCK or SOCK_CLOEXEC; nothing here passes those.

Usage: check-tizen-build.py BUILD_DIR
"""
import json
import os
import re
import sys

REPLACED_BACKENDS = ("ihs_udp_posix", "ihs_thread_sdl")
FORBIDDEN_IMPORTS = {"___syscall5": "open (proxied to the main thread from workers)"}
# Registration function: {argument index: parameter count of the callback type}.
CALLBACK_ARGUMENTS = {
    "IHS_ThreadCreate": {0: 1},  # IHS_ThreadFunction(void *context)
    "IHS_TimerTaskStart": {1: 2, 2: 1},  # IHS_TimerRunFunction(int, void *), IHS_TimerEndFunction(void *)
}
# Immediates of the instructions in the module, by opcode: how many LEB128 values follow, or
# how many bytes for the float constants. br_table and the 0xfc and 0xfe prefixes are decoded
# on their own.
NO_IMMEDIATES = {0x00, 0x01, 0x05, 0x0b, 0x0f, 0x1a, 0x1b, *range(0x45, 0xc5)}
LEB_IMMEDIATES = {**dict.fromkeys((0x02, 0x03, 0x04, 0x0c, 0x0d, 0x10, 0x3f, 0x40, 0x41, 0x42), 1),
                  **dict.fromkeys(range(0x20, 0x25), 1), **dict.fromkeys(range(0x28, 0x3f), 2), 0x11: 2}
BYTE_IMMEDIATES = {0x43: 4, 0x44: 8}
# After 0xfc: the saturating truncations, then the bulk memory and table instructions.
MISC_LEB_IMMEDIATES = {**dict.fromkeys(range(8), 0), 8: 2, 9: 1, 10: 2, 11: 1, 12: 2, 13: 1, 14: 2, 15: 1,
                       16: 1, 17: 1}


def archive_members(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"!<arch>\n"):
        raise ValueError(f"{path}: not an ar archive")
    i, longnames, names = 8, b"", []
    while i + 60 <= len(data):
        header = data[i:i + 60]
        name = header[:16].decode().strip()
        size = int(header[48:58].decode().strip())
        body = data[i + 60:i + 60 + size]
        if name == "//":
            longnames = body
        elif name.startswith("/") and name[1:].isdigit():
            start = int(name[1:])
            end = longnames.index(b"/\n", start)
            names.append(longnames[start:end].decode())
        elif name not in ("/", "/SYM64/"):
            names.append(name.rstrip("/"))
        i += 60 + size + (size & 1)
    return names


def leb128(data, i):
    result = shift = 0
    while True:
        byte = data[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return result, i


def wasm_imports(path):
    with open(path, "rb") as f:
        data = f.read()
    i = 8
    while i < len(data):
        section, i = data[i], i + 1
        size, i = leb128(data, i)
        end = i + size
        if section == 2:
            count, i = leb128(data, i)
            for _ in range(count):
                length, i = leb128(data, i)
                i += length
                length, i = leb128(data, i)
                name = data[i:i + length].decode()
                i += length
                kind, i = data[i], i + 1
                if kind == 0:
                    _, i = leb128(data, i)
                elif kind in (1, 2):
                    if kind == 1:
                        i += 1
                    flags, i = leb128(data, i)
                    _, i = leb128(data, i)
                    if flags & 1:
                        _, i = leb128(data, i)
                elif kind == 3:
                    i += 2
                yield name
            return
        i = end


def skip_leb128(data, i, count):
    for _ in range(count):
        _, i = leb128(data, i)
    return i


def is_i64_atomic(op):
    """After 0xfe: memory.atomic.wait64, and the i64 forms in each group of seven from 0x10
    (load, store, then each read-modify-write: i32, i64, i32 8 and 16 bits, i64 8, 16 and 32)."""
    return op == 0x02 or (0x10 <= op <= 0x4e and (op - 0x10) % 7 in (1, 4, 5, 6))


def wasm_i64_atomics(path):
    """Decodes every function body; returns the 64-bit atomic instructions and the bodies."""
    with open(path, "rb") as f:
        data = f.read()
    i = 8
    while i < len(data):
        section, i = data[i], i + 1
        size, i = leb128(data, i)
        end = i + size
        if section == 10:
            count, i = leb128(data, i)
            found = 0
            for _ in range(count):
                size, i = leb128(data, i)
                body_end = i + size
                groups, i = leb128(data, i)
                for _ in range(groups):
                    _, i = leb128(data, i)
                    i += 1
                while i < body_end:
                    op, i = data[i], i + 1
                    if op in NO_IMMEDIATES:
                        continue
                    if op in LEB_IMMEDIATES:
                        i = skip_leb128(data, i, LEB_IMMEDIATES[op])
                    elif op in BYTE_IMMEDIATES:
                        i += BYTE_IMMEDIATES[op]
                    elif op == 0x0e:
                        targets, i = leb128(data, i)
                        i = skip_leb128(data, i, targets + 1)
                    elif op == 0xfc:
                        sub, i = leb128(data, i)
                        if sub not in MISC_LEB_IMMEDIATES:
                            raise ValueError(f"{path}: unknown instruction 0xfc {sub} at byte {i}")
                        i = skip_leb128(data, i, MISC_LEB_IMMEDIATES[sub])
                    elif op == 0xfe:
                        sub, i = leb128(data, i)
                        if sub > 0x4e or 0x03 < sub < 0x10:
                            raise ValueError(f"{path}: unknown instruction 0xfe {sub:#x} at byte {i}")
                        i = skip_leb128(data, i, 1 if sub == 0x03 else 2)
                        found += is_i64_atomic(sub)
                    else:
                        raise ValueError(f"{path}: unknown opcode {op:#x} at byte {i - 1}")
                if i != body_end:
                    raise ValueError(f"{path}: a function body ends at byte {i}, not {body_end}")
            return found, count
        i = end
    return 0, 0


def call_arguments(text, start):
    """Splits the argument list of the call whose "(" is at text[start]."""
    depth, args, current = 0, [], ""
    for ch in text[start:]:
        if ch == "(":
            depth += 1
            if depth == 1:
                continue
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(current.strip())
                return args
        elif ch == "," and depth == 1:
            args.append(current.strip())
            current = ""
            continue
        current += ch
    return args


def parameter_count(params):
    params = params.strip()
    return 0 if params in ("", "void") else params.count(",") + 1


def callback_mismatches(build):
    with open(os.path.join(build, "compile_commands.json")) as f:
        sources = sorted({e["file"] for e in json.load(f) if e["file"].endswith(".c")})
    texts = {}
    for path in sources:
        with open(path, errors="replace") as f:
            texts[path] = re.sub(r"/\*.*?\*/|//[^\n]*", "", f.read(), flags=re.S)
    errors, checked = [], 0
    for path, text in texts.items():
        for registrar, expected in CALLBACK_ARGUMENTS.items():
            for m in re.finditer(r"\b%s\s*\(" % registrar, text):
                args = call_arguments(text, m.end() - 1)
                for index, count in expected.items():
                    if index >= len(args):
                        continue
                    # Drop casts such as (IHS_ThreadFunction *); a declaration's
                    # "IHS_ThreadFunction *function" is not a bare name and is skipped.
                    name = re.sub(r"^(\([^()]*\)\s*)+", "", args[index]).lstrip("&").strip()
                    if not re.fullmatch(r"[A-Za-z_]\w*", name) or name == "NULL":
                        continue
                    checked += 1
                    definition = re.compile(r"\b%s\s*\(([^)]*)\)\s*\{" % name)
                    found = definition.search(text) or next(
                        (d for d in map(definition.search, texts.values()) if d), None)
                    if found is None:
                        errors.append(f"{path}: no definition found for {registrar} callback {name}")
                    elif parameter_count(found.group(1)) != count:
                        errors.append(f"{path}: {registrar} callback {name}({found.group(1).strip()}) "
                                      f"takes {parameter_count(found.group(1))} parameters, "
                                      f"its callback type {count}")
    return errors, checked


def main():
    build = sys.argv[1]
    errors = []
    members = archive_members(os.path.join(build, "libihslib.a"))
    for member in members:
        if member.startswith(REPLACED_BACKENDS):
            errors.append(f"libihslib.a contains {member}, a backend this project replaces")
    wasm = os.path.join(build, "src", "tizen", "tsl.wasm")
    imports = set(wasm_imports(wasm))
    for name, why in FORBIDDEN_IMPORTS.items():
        if name in imports:
            errors.append(f"tsl.wasm imports {name}: {why}")
    i64_atomics, functions = wasm_i64_atomics(wasm)
    if i64_atomics:
        errors.append(f"tsl.wasm has {i64_atomics} 64-bit atomic instructions, which hang Tizen 5.5 "
                      "(std::atomic<uint64_t> makes them; use Counter64)")
    mismatches, callbacks = callback_mismatches(build)
    errors += mismatches
    for e in errors:
        print("check-tizen-build: " + e, file=sys.stderr)
    if errors:
        return 1
    print(f"check-tizen-build: ok ({len(members)} ihslib objects, {len(imports)} wasm imports, "
          f"{functions} functions without 64-bit atomics, {callbacks} thread and timer callbacks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
