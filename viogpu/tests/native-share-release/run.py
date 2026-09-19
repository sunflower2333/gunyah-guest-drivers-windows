#!/usr/bin/env python3
"""Fault-inject the actual release callback, including failed-release retries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'viogpuwddm/wddmddi.cpp').read_text()
start = source.index('static NTSTATUS ReleaseNativeShareLocked(')
end = source.index('\nstatic NTSTATUS HandleNativeShareEscapeLocked(', start)
body = source[start:end]
fixture = r'''
#include <cassert>
#include <cstddef>
#include <cstdio>
using NTSTATUS=int; using ULONG=unsigned;
#define _In_
#define _Out_
constexpr int STATUS_SUCCESS=0, STATUS_DEVICE_NOT_READY=-1, STATUS_INVALID_PARAMETER=-2;
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted, VioGpuHostContextConfirmed, VioGpuHostContextUnknown };
struct LIST_ENTRY { LIST_ENTRY *Flink,*Blink; };
using PLIST_ENTRY=LIST_ENTRY*;
#define CONTAINING_RECORD(p,t,f) reinterpret_cast<t*>(reinterpret_cast<char*>(p)-offsetof(t,f))
void RemoveEntryList(LIST_ENTRY *p) { p->Blink->Flink=p->Flink; p->Flink->Blink=p->Blink; }
LIST_ENTRY g_VioGpuNativeImports;
struct VIOGPU_WDDM_CONTEXT {};
struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT;
struct VioGpuDod {
 bool admitted=true; int acquired=0,released=0,calls=0;
 VIOGPU_HOST_CONTEXT_RESULT result=VioGpuHostContextConfirmed;
 bool AcquireNativeSubmissionOperation() { if(admitted) ++acquired; return admitted; }
 void ReleaseNativeSubmissionOperation() { ++released; }
 VIOGPU_HOST_CONTEXT_RESULT ReleaseNativeSharedResource(const VIOGPU_NATIVE_CONTEXT_SNAPSHOT*,unsigned id) {
  assert(id==99); ++calls; return result;
 }
};
struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT { VioGpuDod *Adapter; };
struct VIOGPU_WDDM_NATIVE_SHARE { unsigned ShareKey,Iova; };
int deleted;
struct VIOGPU_WDDM_NATIVE_IMPORT_ENTRY {
 LIST_ENTRY Link; VIOGPU_WDDM_CONTEXT *Context; VioGpuDod *Adapter;
 unsigned Key,Iova,ResourceId;
 ~VIOGPU_WDDM_NATIVE_IMPORT_ENTRY() { ++deleted; }
};
FUNCTION
int main() {
 int cases=0;
 for(int failure=0;failure<3;++failure) {
  VioGpuDod adapter,other; VIOGPU_WDDM_CONTEXT context,wrongContext;
  VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot{&adapter};
  VIOGPU_WDDM_NATIVE_SHARE request{11,4096}; ULONG stage=999;
  auto *entry=new VIOGPU_WDDM_NATIVE_IMPORT_ENTRY{{},&context,&adapter,11,4096,99};
  g_VioGpuNativeImports={&entry->Link,&entry->Link};
  entry->Link={&g_VioGpuNativeImports,&g_VioGpuNativeImports}; deleted=0;
  auto release=[&](VioGpuDod *a,VIOGPU_WDDM_CONTEXT *c) {
   return ReleaseNativeShareLocked(a,c,&snapshot,&request,&stage);
  };
  assert(release(&other,&context)==STATUS_INVALID_PARAMETER && stage==41); ++cases;
  assert(release(&adapter,&wrongContext)==STATUS_INVALID_PARAMETER && stage==41); ++cases;
  request.ShareKey=12;
  assert(release(&adapter,&context)==STATUS_INVALID_PARAMETER && stage==41); ++cases;
  request.ShareKey=11; request.Iova=8192;
  assert(release(&adapter,&context)==STATUS_INVALID_PARAMETER && stage==41); ++cases;
  request.Iova=4096;
  assert(adapter.calls==0 && deleted==0);
  adapter.admitted=failure!=0;
  adapter.result=failure==2?VioGpuHostContextUnknown:VioGpuHostContextNotSubmitted;
  for(int repeat=0;repeat<2;++repeat) {
   assert(release(&adapter,&context)==STATUS_DEVICE_NOT_READY && stage==42);
   assert(deleted==0 && g_VioGpuNativeImports.Flink==&entry->Link);
   assert(entry->Key==11 && entry->Iova==4096);
   assert(adapter.acquired==adapter.released); ++cases;
  }
  assert(adapter.calls==(failure==0?0:2));
  adapter.admitted=true; adapter.result=VioGpuHostContextConfirmed;
  assert(release(&adapter,&context)==STATUS_SUCCESS && stage==0); ++cases;
  assert(deleted==1 && g_VioGpuNativeImports.Flink==&g_VioGpuNativeImports);
  assert(adapter.acquired==adapter.released);
  int calls=adapter.calls;
  assert(release(&adapter,&context)==STATUS_INVALID_PARAMETER && stage==41); ++cases;
  assert(deleted==1 && adapter.calls==calls);
 }
 std::printf("production share release: %d cases PASS\n",cases);
}
'''
anchor = 'if (result == VioGpuHostContextConfirmed)\n        {'
assert anchor in body
negative = body.replace(anchor, 'if (true)\n        {', 1)
with tempfile.TemporaryDirectory(prefix='native-share-release-') as tmp:
    tmp = Path(tmp)
    for name, code in [('production', body), ('lost-reservation', negative)]:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text(fixture.replace('FUNCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'deleted==0' in result.stderr, result.stderr
            print('negative control: lost reservation detected')
