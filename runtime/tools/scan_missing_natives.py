#!/usr/bin/env python3
"""
Scan every JAR under games/ for references to javax/microedition methods
and report which ones the emulator doesn't yet register. Useful for
discovering missing natives when a specific title misbehaves.

Usage:
    runtime/tools/scan_missing_natives.py            # report all JARs
    runtime/tools/scan_missing_natives.py <jar>      # one JAR in detail

Much faster than per-class javap — parses the constant pool directly.
"""
import os, re, struct, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_ROOT = os.path.abspath(os.path.join(HERE, '..', 'src'))

# Parse registered natives out of the C++ source. Matches register_native(),
# register_noop() and register_stub() — all take (class, name, desc) as the
# first three string args (noop/stub have a 4th `note` arg that we ignore).
_REG = re.compile(
    r'register_(?:native|noop|stub)\(\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"',
    re.DOTALL)

# Loop pattern:  for (const char* IDENT : { "STR1", "STR2", ... }) { BODY }
# When the loop variable is later passed as the first arg to register_native(),
# expand to one row per class string. Pretty common in this codebase for
# registering the same method on multiple class names (List + ChoiceGroup,
# TextField + TextBox, exception subclasses, etc.).
_LOOP = re.compile(
    r'for\s*\(\s*(?:const\s+)?char\s*\*\s*(\w+)\s*:\s*\{([^}]+)\}\s*\)\s*\{',
    re.DOTALL)
_REG_VAR = lambda var: re.compile(
    r'register_(?:native|noop|stub)\(\s*' + re.escape(var) +
    r'\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"',
    re.DOTALL)

def _matching_brace(text, open_pos):
    depth = 1; i = open_pos + 1
    while i < len(text) and depth:
        c = text[i]
        if c == '{': depth += 1
        elif c == '}': depth -= 1
        i += 1
    return i  # index just past the matching close

def load_registered():
    out = set()
    str_lit = re.compile(r'"([^"]+)"')
    for root, _, files in os.walk(SRC_ROOT):
        for f in files:
            if not f.endswith(('.cpp', '.hpp', '.h')):
                continue
            text = open(os.path.join(root, f)).read()
            # Direct literal-arg registrations
            for cls, name, desc in _REG.findall(text):
                out.add(f"{cls}.{name}{desc}")
            # Loop-variable-arg registrations
            for m in _LOOP.finditer(text):
                var = m.group(1)
                classes = str_lit.findall(m.group(2))
                if not classes: continue
                body_start = m.end() - 1  # the `{`
                body_end = _matching_brace(text, body_start)
                body = text[body_start:body_end]
                for name, desc in _REG_VAR(var).findall(body):
                    for cls in classes:
                        out.add(f"{cls}.{name}{desc}")
    return out

# Minimal class-file parser: walks the constant pool and emits every
# Methodref whose class starts with javax/microedition/.
def methods_of_classfile(data):
    # Skip magic + minor + major
    off = 10
    cp_count = struct.unpack('>H', data[8:10])[0]
    cp = [None] * cp_count
    i = 1
    while i < cp_count:
        tag = data[off]; off += 1
        if tag == 1:  # Utf8
            ln = struct.unpack('>H', data[off:off+2])[0]; off += 2
            cp[i] = data[off:off+ln].decode('utf-8', 'replace')
            off += ln
        elif tag in (3, 4):  # Integer, Float
            cp[i] = struct.unpack('>I', data[off:off+4])[0]; off += 4
        elif tag in (5, 6):  # Long, Double (take two slots)
            cp[i] = struct.unpack('>Q', data[off:off+8])[0]; off += 8
            i += 1  # skip extra slot
        elif tag == 7:  # Class
            cp[i] = ('Class', struct.unpack('>H', data[off:off+2])[0]); off += 2
        elif tag == 8:  # String
            cp[i] = ('String', struct.unpack('>H', data[off:off+2])[0]); off += 2
        elif tag in (9, 10, 11):  # Fieldref, Methodref, InterfaceMethodref
            cls_i, nat_i = struct.unpack('>HH', data[off:off+4])
            cp[i] = (('Field','Method','IfaceMethod')[tag-9], cls_i, nat_i)
            off += 4
        elif tag == 12:  # NameAndType
            n, d = struct.unpack('>HH', data[off:off+4])
            cp[i] = ('NameAndType', n, d); off += 4
        elif tag in (15, 16, 18):  # MethodHandle / MethodType / InvokeDynamic
            off += {15: 3, 16: 2, 18: 4}[tag]
        else:
            raise ValueError(f"Unknown CP tag {tag}")
        i += 1
    out = set()
    for e in cp:
        if not isinstance(e, tuple): continue
        if e[0] not in ('Method', 'IfaceMethod'): continue
        _, cls_i, nat_i = e
        klass = cp[cp[cls_i][1]]
        n, d = cp[nat_i][1], cp[nat_i][2]
        name = cp[n]; desc = cp[d]
        if klass.startswith('javax/microedition/'):
            out.add(f"{klass}.{name}{desc}")
    return out

def scan_jar(jar_path):
    methods = set()
    with zipfile.ZipFile(jar_path) as zf:
        for n in zf.namelist():
            if not n.endswith('.class'): continue
            try:
                methods |= methods_of_classfile(zf.read(n))
            except Exception as e:
                pass
    return methods

def main():
    registered = load_registered()
    if len(sys.argv) > 1:
        jar = sys.argv[1]
        used = scan_jar(jar)
        missing = used - registered
        print(f"{jar}: {len(used)} used, {len(missing)} missing")
        for m in sorted(missing):
            print(f"  {m}")
        return
    root = 'games'
    per_jar = {}
    for dp, _, files in os.walk(root):
        for f in files:
            if not f.endswith('.jar'): continue
            # skip (a), (a1), (a2) duplicates
            if re.search(r'\(a\d*\)\.jar$', f): continue
            jar = os.path.join(dp, f)
            try:
                used = scan_jar(jar)
                missing = used - registered
                per_jar[jar] = (used, missing)
            except Exception:
                pass
    # Union of all missing methods, with how many JARs use each
    all_missing = {}
    for jar, (_, missing) in per_jar.items():
        for m in missing:
            all_missing.setdefault(m, []).append(os.path.basename(jar))
    print(f"{len(registered)} natives registered, {len(per_jar)} JARs scanned\n")
    print("Per-JAR summary (sorted by missing count):")
    for jar, (used, missing) in sorted(per_jar.items(), key=lambda x: -len(x[1][1])):
        print(f"  {os.path.basename(jar):60} {len(used):3} used, {len(missing):2} missing")
    print(f"\nTop missing methods across all JARs:")
    for m, jars in sorted(all_missing.items(), key=lambda x: -len(x[1]))[:30]:
        print(f"  [{len(jars):2}] {m}")

if __name__ == '__main__':
    main()
