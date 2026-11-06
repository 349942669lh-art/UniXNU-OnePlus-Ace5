"""Extract pinned Linux Command DB/ARC helpers for valid host fixtures only.

Original GPL sources stay in the host oracle, never in generated XNU modules.
Upstream lookup lacks bounds checks: NEVER call it on malformed/unaligned data.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import urllib.request
ROOT = Path(__file__).resolve().parents[1]


def extract_function(text, signature):
    assert text.count(signature) == 1, signature
    start = text.index(signature)
    end = text.index('\n}', start) + 2
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fetch', action='store_true')
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    pins = json.loads((ROOT / 'hardware/gpu-arc-source.json').read_text())
    texts = {}
    for entry in pins['files']:
        path = ROOT / '.cache/gpu-arc-source' / entry['path']
        if not path.exists() and a.fetch:
            with urllib.request.urlopen(entry['url'], timeout=60) as response:
                data = response.read(entry['bytes'] + 1)
            if len(data) != entry['bytes'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise ValueError('Pinned ARC source download mismatch: ' + entry['path'])
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        data = path.read_bytes()
        if len(data) != entry['bytes'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise ValueError('Pinned ARC source hash mismatch: ' + entry['path'])
        texts[Path(entry['path']).name] = data.decode()
    db, arc = texts['cmd-db.c'], texts['rpmh-regulator.c']
    structs = []
    for name in ('entry_header', 'rsc_hdr', 'cmd_db_header'):
        matches = re.findall(r'^struct ' + name + r' \{[\s\S]*?^\};', db, re.M)
        assert len(matches) == 1, name
        structs.extend(matches)
    functions = [extract_function(db, signature) for signature in (
        'static bool cmd_db_magic_matches(',
        'static inline const void *rsc_to_entry_header(',
        'static inline void *\nrsc_offset(',
        'int cmd_db_ready(',
        'static int cmd_db_get_header(',
        'u32 cmd_db_read_addr(',
        'const void *cmd_db_read_aux_data(',
    )]
    functions.append(extract_function(arc, 'static int\nrpmh_regulator_load_arc_level_mapping('))
    prelude = '''/* SPDX-License-Identifier: GPL-2.0-only
 * Host-only extraction from pinned public OnePlus Linux sources.
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * Original unbounded cmd-db lookup is ONLY for known-valid aligned fixtures.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Oracle fixture supports little-endian hosts only
#endif
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint16_t __le16; typedef uint32_t __le32;
#define NUM_PRIORITY 2
#define MAX_SLV_ID 8
#define RPMH_ARC_MAX_LEVELS 16
#define RPMH_ARC_LEVEL_SIZE 2
#define EPROBE_DEFER 517
#define __nonstring
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define PTR_ERR(x) ((long)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x)>=(uintptr_t)-4095)
#define aggr_vreg_err(...) ((void)0)
#define aggr_vreg_debug(...) ((void)0)
struct rpmh_aggr_vreg {const char *resource_name; int level_count; u32 level[16];};
'''
    globals_ = '''
static struct cmd_db_header *cmd_db_header;
static const u8 CMD_DB_MAGIC[]={0xdb,0x30,0x03,0x0c};
_Static_assert(sizeof(struct cmd_db_header)==144,"original DB header size");
_Static_assert(sizeof(struct rsc_hdr)==16,"original slave header size");
_Static_assert(sizeof(struct entry_header)==24,"original entry size");
_Static_assert(offsetof(struct entry_header,addr)==16,"original address offset");
_Static_assert(offsetof(struct entry_header,len)==20,"original aux length offset");
_Static_assert(offsetof(struct entry_header,offset)==22,"original aux offset");
'''
    harness = '''
int arc_source_resolve(const void *image,const char *name,uint32_t *address,
                       uint16_t *levels,unsigned *count) {
 struct rpmh_aggr_vreg v={0};v.resource_name=name;
 cmd_db_header=(struct cmd_db_header *)image;
 *address=cmd_db_read_addr(name);if(!*address)return -1;
 int code=rpmh_regulator_load_arc_level_mapping(&v);if(code)return code;
 *count=(unsigned)v.level_count;
 for(unsigned i=0;i<*count;i++)levels[i]=(uint16_t)v.level[i];
 cmd_db_header=NULL;return 0;
}
'''
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(prelude + '\n'.join(structs) + globals_ + '\n'.join(functions) + harness,
                     encoding='utf-8', newline='\n')
    print('Pinned Linux Command DB lookup and ARC AUX mapping extracted; valid host fixtures only, not XNU code')


if __name__ == '__main__':
    main()
