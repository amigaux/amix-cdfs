#!/usr/bin/env python3
# modgate.py -- the cdfs module's own gate, after the ABI kit's modcheck.py.
#
#   modgate.py KIT MODULE
#
# - every undefined global (weak ones included) is a DDI name of the kit's
#   ddi.list, or the wrapper's optional <prefix>_conf_data (weak, 0 when
#   absent); so nothing outside the DDI, whatever modcheck's tier rules allow.
#   Checked twice: from the symbol table, and as `m68k-cbm-sysv4-nm -u` lists
#   it (on a copy whose .moddata is PROGBITS again, since binutils 2.8.1
#   cannot read a section of type 13);
# - fs= equals the name cdfs_mod.c passes to vfs_register (CDFS_FSNAME);
# - the object runs in place (amix-dlm C4-4 section 4): the kit checker's
#   own root-class rule, applied to this boot-class object, so that the same
#   object can be placed by boot2 if a generation promotes it to the root
#   class;
# - prints the .moddata keys and the sha256, one line each.
#
# It uses the kit's modcheck.py (its ELF reader and its in-place rule), so
# the rule is the kit's, not a copy.
#
# SPDX-License-Identifier: BSD-2-Clause
import hashlib, os, re, struct, subprocess, sys, tempfile

def nm_undefined(e, name):
    b = bytearray(e.b)
    shoff, = struct.unpack('>I', b[32:36])
    for i, s in enumerate(e.sh):
        if s['type'] == 13:
            struct.pack_into('>I', b, shoff + 40 * i + 4, 1)
    with tempfile.TemporaryDirectory() as t:
        tmp = os.path.join(t, name)
        open(tmp, 'wb').write(b)
        try:
            r = subprocess.run(['m68k-cbm-sysv4-nm', '-u', tmp], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        except FileNotFoundError:
            return None
    if r.returncode != 0:
        return None
    return sorted(set(l.split()[-1] for l in r.stdout.decode('latin-1').splitlines()
                      if l.strip()))


def main(argv):
    if len(argv) != 2:
        sys.exit('usage: modgate.py KIT MODULE')
    kit, path = argv
    sys.path.insert(0, os.path.join(kit, 'tools'))
    import modcheck as mc
    e = mc.Elf(path)
    name = os.path.basename(path)

    allow = mc.names(os.path.join(kit, 'ddi.list'))
    undef = sorted(set(y['name'] for y in e.syms
                       if y['shndx'] == 0 and y['bind'] in (1, 2) and y['name']))
    extra = [u for u in undef if u not in allow and u != 'cdfs_conf_data']
    for u in extra:
        mc.bad('undefined %s is not a DDI name (%s/ddi.list)' % (u, kit))

    nmu = nm_undefined(e, name)
    if nmu is None:
        mc.bad('m68k-cbm-sysv4-nm -u failed (the pinned cross tools must be on PATH)')
    elif nmu != undef:
        mc.bad('nm -u lists %s, the symbol table %s' % (' '.join(nmu), ' '.join(undef)))

    md = [s for s in e.sh if s['type'] == 13]
    keys = []
    if len(md) == 1:
        data = e.b[md[0]['off']:md[0]['off'] + md[0]['size']]
        keys = [k.decode('latin-1') for k in data[8:].split(b'\0') if k]
    kv = dict(k.split('=', 1) for k in keys if '=' in k)

    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                            'cdfs_mod.c')).read()
    m = re.search(r'#define\s+CDFS_FSNAME\s+"([^"]*)"', src)
    if not m or kv.get('fs') != m.group(1):
        mc.bad('fs=%s, but cdfs_mod.c registers %r' % (kv.get('fs'), m and m.group(1)))

    mc.inplace(e)

    for p in mc.problems:
        print('modgate: %s: %s' % (name, p))
    if mc.problems:
        return 1
    print('modgate: %s: nm -u lists %d names, all DDI but the weak cdfs_conf_data: %s'
          % (name, len(undef), ' '.join(undef)))
    print('modgate: %s: runs in place (C4-4 section 4)' % name)
    print('modgate: %s: .moddata %s' % (name, ' '.join(keys)))
    print('modgate: %s: sha256 %s' % (name, hashlib.sha256(e.b).hexdigest()))
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
