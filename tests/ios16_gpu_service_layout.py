"""Check the real service layout and heap bookkeeping without IOKit or MMIO.

The modeled base reserves 1 KiB; the complete XNU build independently applies
its exact 16 KiB OSMetaClass limit. This test also compiles all service headers
together, rather than testing each header's namespace separately.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
from ios16_sources import include_flags

source = (ROOT/'ios16/gpu/GiuliaGPU.cpp').read_text()
start = source.index('class GiuliaGPU :')
end = source.index('OSDefineMetaClassAndStructors', start)
metadata = source[source.index('struct gpu_secure_metadata {'):source.index('static struct gg_terminal_mailbox')]
headers = re.findall(r'^#include "([^"]+)"', source[:start], re.M)

def method(signature):
    begin = source.index(signature)
    return source[begin:source.index('\n}', begin)+2]

assert source.index('static void gpuHfiBarrier(void *);') < source.index('gpuHfiBarrier(NULL);')
assert 'runtimeLock && allocateSecureMetadata()' in method('bool GiuliaGPU::init(')
assert 'if(secureData)for(' in method('void GiuliaGPU::releaseResources(')
free = method('void GiuliaGPU::free(')
assert free.index('releaseResources();') < free.index('releaseSecureMetadata();') < free.index('IOService::free();')

layout = '''#include <stdint.h>
#include <stddef.h>
#define GIULIA_PLATFORM_BROKER_ENABLED 1
#define OSDeclareDefaultStructors(x)
class OSDictionary; class IOMemoryDescriptor; class IOMemoryMap;
class IOBufferMemoryDescriptor; class IOLock;
typedef void *thread_t; typedef int wait_result_t;
class IOService {
    char modeledBase[1024];
public:
    virtual bool init(OSDictionary *);
    virtual bool start(IOService *);
    virtual void stop(IOService *);
    virtual void free();
};
'''+''.join('#include "'+h+'"\n' for h in headers)+metadata+source[start:end]+'''
static_assert(sizeof(GiuliaGPU) <= 14*1024, "service bookkeeping exceeds modeled layout budget");
static_assert(sizeof(gpu_secure_metadata) == GGS_SEC_SEGMENTS*16, "metadata ABI");
static_assert(GGRC_OK == 0 && GGR_OK == 0, "independent result enums retain their wire values");
'''

fixture = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include "giulia_gpu_platform.h"
class IOBufferMemoryDescriptor;
'''+metadata+r'''
static bool failAllocation;
static unsigned allocated, freed;
static void *IOMalloc(size_t bytes) {
    assert(bytes==sizeof(gpu_secure_metadata));
    if(failAllocation)return nullptr;
    auto *p=std::malloc(bytes);assert(p);std::memset(p,0xa5,bytes);++allocated;return p;
}
static void IOFree(void *p,size_t bytes) {
    assert(p && bytes==sizeof(gpu_secure_metadata));++freed;std::free(p);
}
#define bzero(p,bytes) std::memset(p,0,bytes)
class GiuliaGPU {
public:
    gpu_secure_metadata *secureMetadata=nullptr;
    IOBufferMemoryDescriptor **secureData=nullptr;
    uint64_t *securePA=nullptr;
    bool manualMemoryPinned=false;
    bool allocateSecureMetadata();
    void releaseSecureMetadata();
};
'''+method('bool GiuliaGPU::allocateSecureMetadata(')+'\n'+method('void GiuliaGPU::releaseSecureMetadata(')+r'''
int main() {
    GiuliaGPU s;
    s.releaseSecureMetadata();assert(!freed);
    failAllocation=true;
    assert(!s.allocateSecureMetadata() && !s.secureMetadata && !s.secureData && !s.securePA && !allocated);
    failAllocation=false;
    assert(s.allocateSecureMetadata() && allocated==1 && !freed);
    assert(s.secureData==s.secureMetadata->descriptors && s.securePA==s.secureMetadata->physical);
    for(unsigned i=0;i<GGS_SEC_SEGMENTS;++i)assert(!s.secureData[i] && !s.securePA[i]);
    assert(!s.allocateSecureMetadata() && allocated==1);
    auto *saved=s.secureMetadata;
    s.securePA[GGS_SEC_SEGMENTS-1]=0x123456789ULL;
    s.manualMemoryPinned=true;s.releaseSecureMetadata();
    assert(!freed && s.secureMetadata==saved && s.securePA[GGS_SEC_SEGMENTS-1]==0x123456789ULL);
    // Ordinary RAM fixture cleanup only: no device ownership was installed.
    s.manualMemoryPinned=false;s.releaseSecureMetadata();
    assert(freed==1 && !s.secureMetadata && !s.secureData && !s.securePA);
    s.releaseSecureMetadata();assert(freed==1);
    assert(s.allocateSecureMetadata());
    for(unsigned i=0;i<GGS_SEC_SEGMENTS;++i)assert(!s.secureData[i] && !s.securePA[i]);
    s.releaseSecureMetadata();assert(allocated==freed && freed==2);
    IOBufferMemoryDescriptor *foreign[1]={};s.secureData=foreign;
    assert(!s.allocateSecureMetadata() && allocated==freed);
}
'''

with tempfile.TemporaryDirectory(prefix='gpu-service-layout-') as folder:
    folder = Path(folder)
    unit = folder/'layout.cpp'; unit.write_text(layout)
    cxx = os.environ.get('CXX', 'c++')
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', *include_flags(ROOT)]
    subprocess.run([cxx, *flags, '-fsyntax-only', str(unit)], check=True)
    subprocess.run([cxx, '--target=arm64-apple-macos', '-ffreestanding', '-nostdinc++', *flags,
                    '-fsyntax-only', str(unit)], check=True)
    unit = folder/'lifetime.cpp'; unit.write_text(fixture)
    binary = folder/'lifetime'
    subprocess.run([cxx, *flags, '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('GPU service: combined header namespaces and Darwin layout budget passed; '
      'real heap metadata helpers passed allocation failure, zeroing, duplicate refusal, '
      'retention and idempotent release (ordinary RAM only; exact XNU base checked in CI)')
