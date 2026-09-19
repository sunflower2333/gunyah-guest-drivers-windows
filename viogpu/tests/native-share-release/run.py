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

# Exercise exporter revocation too, with the actual registry traversal and
# injected failures at every admission boundary before the host confirmation.
revoke_start = source.index('NTSTATUS RevokeNativeShares(')
revoke_end = source.index('/* Runs under the context', revoke_start)
revoke = source[revoke_start:revoke_end]
prefix = fixture.split('FUNCTION')[0].replace(
    'struct VIOGPU_WDDM_CONTEXT {};',
    'struct VIOGPU_WDDM_CONTEXT { int Signature=123, Operations=0; };')
prefix = prefix.replace('calls=0;', 'calls=0,failOnCall=0;').replace(
    '++calls; return result;',
    '++calls; return calls==failOnCall?VioGpuHostContextUnknown:result;')
revoke_fixture = prefix + r'''
using UINT=unsigned;
constexpr int STATUS_DEVICE_BUSY=-3, VIOGPU_NATIVE_RESOURCE_ID_START=90;
constexpr int VIOGPU_WDDM_CONTEXT_SIGNATURE=123;
#define PAGED_CODE() ((void)0)
#define FALSE false
struct VIOGPU_WDDM_NATIVE_SHARE_ENTRY {
 LIST_ENTRY Link; VioGpuDod *Adapter; unsigned Key,ResourceId,Size;
};
LIST_ENTRY g_VioGpuNativeShares;
bool registry=true,rundown=true,snapshotOK=true;
int registryHeld=0,rundownHeld=0,snapshotHeld=0;
VioGpuDod *activeAdapter;
bool AcquireNativeShareRegistry(bool) { if(registry) ++registryHeld; return registry; }
void ReleaseNativeShareRegistry() { --registryHeld; }
bool ExAcquireRundownProtection(int*) { if(rundown) ++rundownHeld; return rundown; }
void ExReleaseRundownProtection(int*) { --rundownHeld; }
VIOGPU_WDDM_CONTEXT* NativeRegistration(VIOGPU_WDDM_CONTEXT *c) { return c; }
struct VioGpuAdapter {
 static bool AcquireNativeContextSnapshot(VIOGPU_WDDM_CONTEXT*,VIOGPU_NATIVE_CONTEXT_SNAPSHOT *s) {
  if(snapshotOK) {++snapshotHeld;s->Adapter=activeAdapter;} return snapshotOK;
 }
 static void ReleaseNativeContextSnapshot(VIOGPU_NATIVE_CONTEXT_SNAPSHOT*) {--snapshotHeld;}
};
FUNCTION
int main() {
 int cases=0;
 for(int failure=0;failure<6;++failure) {
  VioGpuDod adapter; activeAdapter=&adapter; VIOGPU_WDDM_CONTEXT context;
  registry=rundown=snapshotOK=true; deleted=0;
  auto *share=new VIOGPU_WDDM_NATIVE_SHARE_ENTRY{{},&adapter,11,99,4096};
  g_VioGpuNativeShares={&share->Link,&share->Link};
  share->Link={&g_VioGpuNativeShares,&g_VioGpuNativeShares};
  auto *entry=new VIOGPU_WDDM_NATIVE_IMPORT_ENTRY{{},&context,&adapter,11,4096,99};
  g_VioGpuNativeImports={&entry->Link,&entry->Link};
  entry->Link={&g_VioGpuNativeImports,&g_VioGpuNativeImports};
  if(failure==0) context.Signature=0;
  if(failure==1) rundown=false;
  if(failure==2) snapshotOK=false;
  if(failure==3) adapter.admitted=false;
  if(failure==4) adapter.result=VioGpuHostContextNotSubmitted;
  if(failure==5) adapter.result=VioGpuHostContextUnknown;
  for(int attempt=0;attempt<2;++attempt) {
   assert(RevokeNativeShares(&adapter,99)==STATUS_DEVICE_BUSY);
   assert(deleted==0 && g_VioGpuNativeImports.Flink==&entry->Link);
   assert(g_VioGpuNativeShares.Flink==&share->Link);
   assert(registryHeld==0 && rundownHeld==0 && snapshotHeld==0);
   assert(adapter.acquired==adapter.released); ++cases;
  }
  context.Signature=123; rundown=snapshotOK=adapter.admitted=true;
  adapter.result=VioGpuHostContextConfirmed;
  assert(RevokeNativeShares(&adapter,99)==STATUS_SUCCESS);
  assert(deleted==1 && g_VioGpuNativeImports.Flink==&g_VioGpuNativeImports);
  assert(g_VioGpuNativeShares.Flink==&g_VioGpuNativeShares);
  assert(registryHeld==0 && rundownHeld==0 && snapshotHeld==0); ++cases;
  assert(adapter.acquired==adapter.released);
  assert(RevokeNativeShares(&adapter,99)==STATUS_SUCCESS); ++cases;
 }
 // Partial progress: the first import is released, the second is uncertain.
 // Keep the exporter and remaining import without replaying the first detach.
 {
  VioGpuDod adapter; activeAdapter=&adapter; adapter.failOnCall=2;
  VIOGPU_WDDM_CONTEXT context; deleted=0;
  auto *share=new VIOGPU_WDDM_NATIVE_SHARE_ENTRY{{},&adapter,11,99,4096};
  g_VioGpuNativeShares={&share->Link,&share->Link};
  share->Link={&g_VioGpuNativeShares,&g_VioGpuNativeShares};
  auto *first=new VIOGPU_WDDM_NATIVE_IMPORT_ENTRY{{},&context,&adapter,11,4096,99};
  auto *last=new VIOGPU_WDDM_NATIVE_IMPORT_ENTRY{{},&context,&adapter,11,8192,99};
  g_VioGpuNativeImports={&first->Link,&last->Link};
  first->Link={&last->Link,&g_VioGpuNativeImports};
  last->Link={&g_VioGpuNativeImports,&first->Link};
  assert(RevokeNativeShares(&adapter,99)==STATUS_DEVICE_BUSY);
  assert(deleted==1 && adapter.calls==2 && g_VioGpuNativeImports.Flink==&last->Link);
  assert(g_VioGpuNativeShares.Flink==&share->Link); ++cases;
  assert(RevokeNativeShares(&adapter,99)==STATUS_SUCCESS);
  assert(deleted==2 && adapter.calls==3 && g_VioGpuNativeImports.Flink==&g_VioGpuNativeImports);
  assert(g_VioGpuNativeShares.Flink==&g_VioGpuNativeShares);
  assert(registryHeld==0 && rundownHeld==0 && snapshotHeld==0);
  assert(adapter.acquired==adapter.released); ++cases;
 }
 std::printf("production share revocation: %d cases PASS\n",cases);
}
'''
guard = 'if (result != VioGpuHostContextConfirmed)'
assert guard in revoke
with tempfile.TemporaryDirectory(prefix='native-share-revoke-') as tmp:
    tmp = Path(tmp)
    for name, code in [('production', revoke),
                       ('ignore-host-release', revoke.replace(guard,
                           'if ((void)result, false)', 1))]:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text(revoke_fixture.replace('FUNCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'STATUS_DEVICE_BUSY' in result.stderr, result.stderr
            print('negative control: unconfirmed exporter revocation detected')
