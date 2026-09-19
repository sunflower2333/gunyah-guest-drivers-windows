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
 bool retired=false,current=true;
 bool IsNativeContextGenerationCurrent(int generation,unsigned reset) {
  return current && !retired && generation==1 && reset==7;
 }
 bool detached=true; int detachCalls=0;
 VIOGPU_HOST_CONTEXT_RESULT detachResult=VioGpuHostContextConfirmed;
 VIOGPU_HOST_CONTEXT_RESULT Detach2DScanoutResource(unsigned id,bool *out,bool native) {
  assert(id==99 && native);++detachCalls;*out=detached;return detachResult;
 }
 int resets=0;
 void RequestHardwareResetAtAnyIrql() { ++resets; }
 bool IsNativeContextResetRetired(unsigned generation) { return retired && generation==7; }
 VIOGPU_HOST_CONTEXT_RESULT result=VioGpuHostContextConfirmed;
 bool AcquireNativeSubmissionOperation() { if(admitted) ++acquired; return admitted; }
 void ReleaseNativeSubmissionOperation() { ++released; }
 VIOGPU_HOST_CONTEXT_RESULT ReleaseNativeSharedResource(const VIOGPU_NATIVE_CONTEXT_SNAPSHOT*,unsigned id) {
  assert(id==99); ++calls; return result;
 }
};
struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT { VioGpuDod *Adapter; unsigned ResetGeneration=7; int Generation=1; unsigned ContextId=2; };
struct VIOGPU_WDDM_NATIVE_SHARE { unsigned ShareKey,Iova,Size=0; };
int deleted;
struct VIOGPU_WDDM_NATIVE_IMPORT_ENTRY {
 LIST_ENTRY Link; VIOGPU_WDDM_CONTEXT *Context; VioGpuDod *Adapter;
 unsigned Key,Iova,ResourceId;
 unsigned ResetGeneration=7;
 unsigned Size=4096;
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
    'struct VIOGPU_WDDM_CONTEXT { int Signature=123, Operations=0, Type=1; VIOGPU_WDDM_CONTEXT *DomainOwner=nullptr; };')
prefix = prefix.replace('calls=0;', 'calls=0,failOnCall=0;').replace(
    '++calls; return result;',
    '++calls; return calls==failOnCall?VioGpuHostContextUnknown:result;')
revoke_fixture = prefix + r'''
using UINT=unsigned;using BOOLEAN=bool;
constexpr bool TRUE=true;
constexpr int STATUS_DEVICE_BUSY=-3, VIOGPU_NATIVE_RESOURCE_ID_START=90;
constexpr int VIOGPU_WDDM_CONTEXT_SIGNATURE=123;
#define PAGED_CODE() ((void)0)
#define FALSE false
struct VIOGPU_WDDM_NATIVE_SHARE_ENTRY {
 LIST_ENTRY Link; VioGpuDod *Adapter; unsigned Key,ResourceId,Size;
 bool ScanoutReferenced=false;
 int Generation=1;unsigned ResetGeneration=7,OwnerContextId=2;
};
LIST_ENTRY g_VioGpuNativeShares;
bool registry=true,rundown=true,snapshotOK=true;
int registryHeld=0,rundownHeld=0,snapshotHeld=0;
unsigned snapshotGeneration=7;
VIOGPU_WDDM_CONTEXT *lastRegistration=nullptr;
VioGpuDod *activeAdapter;
bool AcquireNativeShareRegistry(bool) { if(registry) ++registryHeld; return registry; }
void ReleaseNativeShareRegistry() { --registryHeld; }
bool ExAcquireRundownProtection(int*) { if(rundown) ++rundownHeld; return rundown; }
void ExReleaseRundownProtection(int*) { --rundownHeld; }
VIOGPU_WDDM_CONTEXT* NativeRegistration(VIOGPU_WDDM_CONTEXT *c) { return c->DomainOwner?c->DomainOwner:c; }
struct VioGpuAdapter {
 static bool AcquireNativeContextSnapshot(VIOGPU_WDDM_CONTEXT *c,VIOGPU_NATIVE_CONTEXT_SNAPSHOT *s) {
  lastRegistration=c;
  if(snapshotOK) {++snapshotHeld;s->Adapter=activeAdapter;s->ResetGeneration=snapshotGeneration;} return snapshotOK;
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

lookup_start = source.index('static BOOLEAN AcquireNativeScanoutShare(')
lookup_end = source.index('/* A shared native allocation', lookup_start)
lookup = source[lookup_start:lookup_end]
scanout_fixture = revoke_fixture.split('FUNCTION')[0] + r'''
using ULONGLONG=unsigned long long;using VOID=void;
VIOGPU_WDDM_NATIVE_SHARE_ENTRY *FindNativeShareByKeyLocked(VioGpuDod *adapter,ULONGLONG key) {
 assert(registryHeld==1);
 for(auto *link=g_VioGpuNativeShares.Flink;link!=&g_VioGpuNativeShares;link=link->Flink) {
  auto *share=CONTAINING_RECORD(link,VIOGPU_WDDM_NATIVE_SHARE_ENTRY,Link);
  if(share->Adapter==adapter&&share->Key==key)return share;
 }
 return nullptr;
}
LOOKUP
FUNCTION
int main() {
 int cases=0;
 for(int failure=0;failure<3;++failure) {
  VioGpuDod adapter;
  auto *share=new VIOGPU_WDDM_NATIVE_SHARE_ENTRY{{},&adapter,11,99,4096};
  g_VioGpuNativeShares={&share->Link,&share->Link};
  share->Link={&g_VioGpuNativeShares,&g_VioGpuNativeShares};
  g_VioGpuNativeImports={&g_VioGpuNativeImports,&g_VioGpuNativeImports};
  UINT id=0;ULONGLONG size=0;
  assert(!AcquireNativeScanoutShare(&adapter,12,&id,&size)&&registryHeld==0);++cases;
  assert(AcquireNativeScanoutShare(&adapter,11,&id,&size));
  assert(registryHeld==1&&id==99&&size==4096&&share->ScanoutReferenced);++cases;
  assert(adapter.acquired==adapter.released+1);
  // The real flip caller releases only after bind/flush/latch.
  ReleaseNativeScanoutShare(&adapter);
  assert(adapter.acquired==adapter.released);
  if(failure==0)adapter.detachResult=VioGpuHostContextUnknown;
  if(failure==1)adapter.detachResult=VioGpuHostContextNotSubmitted;
  if(failure==2)adapter.detached=false;
  for(int attempt=0;attempt<2;++attempt) {
   assert(RevokeNativeShares(&adapter,99)==STATUS_DEVICE_BUSY);
   assert(registryHeld==0&&g_VioGpuNativeShares.Flink==&share->Link&&share->ScanoutReferenced);++cases;
  }
  adapter.detachResult=VioGpuHostContextConfirmed;adapter.detached=true;
  assert(RevokeNativeShares(&adapter,99)==0&&registryHeld==0);
  assert(adapter.detachCalls==3&&g_VioGpuNativeShares.Flink==&g_VioGpuNativeShares);++cases;
 }
 for(int failure=0;failure<3;++failure) {
  VioGpuDod adapter;
  auto *share=new VIOGPU_WDDM_NATIVE_SHARE_ENTRY{{},&adapter,11,99,4096};
  g_VioGpuNativeShares={&share->Link,&share->Link};
  share->Link={&g_VioGpuNativeShares,&g_VioGpuNativeShares};
  g_VioGpuNativeImports={&g_VioGpuNativeImports,&g_VioGpuNativeImports};
  if(failure==0)adapter.admitted=false;
  if(failure==1)adapter.current=false;
  if(failure==2)adapter.retired=true;
  UINT id=123;ULONGLONG size=123;
  assert(!AcquireNativeScanoutShare(&adapter,11,&id,&size));
  assert(!registryHeld&&id==0&&size==0&&!share->ScanoutReferenced);
  assert(adapter.acquired==adapter.released);++cases;
  share->ScanoutReferenced=true;
  if(failure!=2) {
   assert(RevokeNativeShares(&adapter,99)==STATUS_DEVICE_BUSY);
   assert(g_VioGpuNativeShares.Flink==&share->Link&&adapter.detachCalls==0);++cases;
  }
  // Confirmed old-generation retirement must never unbind a new generation.
  adapter.retired=true;
  assert(RevokeNativeShares(&adapter,99)==0&&adapter.detachCalls==0);
  assert(!registryHeld&&adapter.acquired==adapter.released);++cases;
 }
 printf("production native scanout share ownership: %d cases PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='native-scanout-share-') as tmp:
    tmp = Path(tmp)
    for name, lookup_code, revoke_code in [
        ('production', lookup, revoke),
        ('early-unlock', lookup.replace('return TRUE;', 'ReleaseNativeShareRegistry(); return TRUE;'), revoke),
        ('early-rundown', lookup.replace('return TRUE;', 'adapter->ReleaseNativeSubmissionOperation(); return TRUE;'), revoke),
        ('stale-generation', lookup.replace('if (adapter->IsNativeContextGenerationCurrent(share->Generation, share->ResetGeneration))', 'if (true)'), revoke),
        ('skip-detach', lookup, revoke.replace('if (share->ScanoutReferenced)', 'if (false)')),
    ]:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text(scanout_fixture.replace('LOOKUP', lookup_code).replace('FUNCTION', revoke_code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            expected = {'early-unlock':'registryHeld==1', 'early-rundown':'adapter.acquired==adapter.released+1',
                        'stale-generation':'!AcquireNativeScanoutShare', 'skip-detach':'STATUS_DEVICE_BUSY'}[name]
            assert result.returncode != 0 and expected in result.stderr, result.stderr
            print('negative control detected: ' + name)

export_start = source.index('static NTSTATUS ExportNativeShareLocked(')
export_end = source.index('\nstatic NTSTATUS ImportNativeShareLocked(', export_start)
export = source[export_start:export_end]
export_fixture = revoke_fixture.split('FUNCTION')[0] + r'''
#include <cstring>
#include <new>
using ULONGLONG=unsigned long long;
#define _Inout_
constexpr UINT MAXUINT=~0u;
constexpr int NonPagedPoolNx=1,STATUS_NO_MEMORY=-10;
void *operator new(std::size_t size,int) {return ::operator new(size);}
void RtlZeroMemory(void *p,std::size_t size) {std::memset(p,0,size);}
void InsertTailList(LIST_ENTRY *head,LIST_ENTRY *entry) {
 entry->Flink=head;entry->Blink=head->Blink;head->Blink->Flink=entry;head->Blink=entry;
}
void FindNativeAllocationRangeByIova(VIOGPU_WDDM_CONTEXT*,unsigned,unsigned,UINT *id,ULONGLONG *size) {
 *id=99;*size=4096;
}
unsigned NewNativeShareKeyLocked(VioGpuDod*) {return 11;}
FUNCTION
int main() {
 int cases=0;
 for(int changed=0;changed<4;++changed) {
  VioGpuDod adapter;VIOGPU_WDDM_CONTEXT context;
  VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot{&adapter};
  VIOGPU_WDDM_NATIVE_SHARE request{0,4096};ULONG stage=0;
  g_VioGpuNativeShares={&g_VioGpuNativeShares,&g_VioGpuNativeShares};
  assert(ExportNativeShareLocked(&adapter,&context,&snapshot,&request,&stage)==0);
  auto *share=CONTAINING_RECORD(g_VioGpuNativeShares.Flink,VIOGPU_WDDM_NATIVE_SHARE_ENTRY,Link);
  assert(request.ShareKey==11&&request.Size==4096&&share->Generation==1&&share->ResetGeneration==7);
  if(changed==1)++snapshot.Generation;
  if(changed==2)++snapshot.ResetGeneration;
  if(changed==3)++snapshot.ContextId;
  request.ShareKey=0;
  int result=ExportNativeShareLocked(&adapter,&context,&snapshot,&request,&stage);
  assert((result==0)==(changed==0));
  assert(share->Generation==1&&share->ResetGeneration==7&&share->OwnerContextId==2);
  assert(request.ShareKey==(changed==0?11u:0u));
  if(changed)assert(stage==23);
  RemoveEntryList(&share->Link);delete share;++cases;
 }
 printf("production native export identity: %d cases PASS\n",cases);
}
'''
guard_start = export.index('    else if (share->Generation')
guard_end = export.index('\n    {', guard_start)
unguarded_export = export[:guard_start] + '    else if (false)' + export[guard_end:]
with tempfile.TemporaryDirectory(prefix='native-export-identity-') as tmp:
    tmp = Path(tmp)
    for name, code in [('production', export), ('retarget-key', unguarded_export)]:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text(export_fixture.replace('FUNCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and '(result==0)==(changed==0)' in result.stderr, result.stderr
            print('negative control detected: retargeted native share key')

import_start = source.index('static NTSTATUS ImportNativeShareLocked(')
import_end = source.index('    if (share != NULL && share->Size == request->Size', import_start)
import_prefix = source[import_start:import_end] + '\n    return STATUS_SUCCESS;\n}\n'
import_fixture = scanout_fixture.split('LOOKUP')[0] + r'''
#define _Inout_
FUNCTION
int main() {
 VioGpuDod adapter;VIOGPU_WDDM_CONTEXT context;
 VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot{&adapter};
 VIOGPU_WDDM_NATIVE_SHARE request{11,4096};
 VIOGPU_WDDM_NATIVE_SHARE_ENTRY share{{},&adapter,11,99,4096};
 g_VioGpuNativeShares={&share.Link,&share.Link};
 share.Link={&g_VioGpuNativeShares,&g_VioGpuNativeShares};registryHeld=1;
 for(int changed=0;changed<3;++changed) {
  share.Generation=changed==1?2:1;share.ResetGeneration=changed==2?8:7;
  ULONG stage=0,host=0,owner=0,id=0;ULONGLONG size=0;
  int status=ImportNativeShareLocked(&adapter,&context,&snapshot,&request,&stage,&size,&host,&owner,&id);
  assert((status==STATUS_SUCCESS)==(changed==0));
  if(changed)assert(stage==30);
 }
 puts("production import generation admission: 3 cases PASS (before alias/attach)");
}
'''
# The fixture stops at the actual pre-alias gate; silence parameters consumed
# by the unextracted attachment path, not compiler warnings in production.
import_prefix = import_prefix.replace('    return STATUS_SUCCESS;',
    '    (void)context; (void)hostResult; (void)snapshot; return STATUS_SUCCESS;')
with tempfile.TemporaryDirectory(prefix='native-import-generation-') as tmp:
    tmp = Path(tmp)
    for name, code in [('production', import_prefix),
                       ('stale-import', import_prefix.replace('share->Generation != snapshot->Generation || share->ResetGeneration != snapshot->ResetGeneration', 'false'))]:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text(import_fixture.replace('FUNCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, cwd=tmp)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and '(status==STATUS_SUCCESS)==(changed==0)' in result.stderr, result.stderr
            print('negative control detected: cross-generation import')

cleanup_start = source.index('NTSTATUS RemoveNativeImportsForContext(')
cleanup_end = source.index('/* Resolve a zero-copy share key', cleanup_start)
cleanup = source[cleanup_start:cleanup_end]
caller_mid = source.index('NTSTATUS importStatus = RemoveNativeImportsForContext(context);')
caller_start = source.rfind('    if (context->Type == VioGpuWddmContextNative)', 0, caller_mid)
caller_end = source.index('    if (context->DomainOwner != NULL)', caller_mid)
caller = source[caller_start:caller_end]
cleanup_fixture = revoke_fixture.split('FUNCTION')[0] + r'''
FUNCTION
#define NT_SUCCESS(x) ((x)>=0)
constexpr int VioGpuWddmContextNative=1;
int destroyPrefix(VIOGPU_WDDM_CONTEXT *context,VioGpuDod *adapter) {
CALLER
 return STATUS_SUCCESS;
}
int main() {
 int cases=0;
 for(bool domain: {false,true}) for(int failure=0;failure<5;++failure) {
  VioGpuDod adapter; activeAdapter=&adapter;
  VIOGPU_WDDM_CONTEXT context,owner;
  if(domain) context.DomainOwner=&owner;
  auto *entry=new VIOGPU_WDDM_NATIVE_IMPORT_ENTRY{{},&context,&adapter,11,4096,99};
  g_VioGpuNativeImports={&entry->Link,&entry->Link};
  entry->Link={&g_VioGpuNativeImports,&g_VioGpuNativeImports};
  registry=true; rundown=false; snapshotOK=true; snapshotGeneration=7; deleted=0;
  if(failure==0) snapshotOK=false;
  if(failure==1) snapshotGeneration=8;
  if(failure==2) adapter.admitted=false;
  if(failure==3) adapter.result=VioGpuHostContextNotSubmitted;
  if(failure==4) adapter.result=VioGpuHostContextUnknown;
  assert(destroyPrefix(&context,&adapter)==STATUS_DEVICE_BUSY && adapter.resets==1);
  assert(deleted==0 && g_VioGpuNativeImports.Flink==&entry->Link);
  assert(lastRegistration==(domain?&owner:&context));
  assert(registryHeld==0 && rundownHeld==0 && snapshotHeld==0); ++cases;
  // A reset request alone is not retirement: another retry still retains it.
  assert(RemoveNativeImportsForContext(&context)==STATUS_DEVICE_BUSY);
  assert(deleted==0); ++cases;
  int calls=adapter.calls;
  adapter.retired=true; // Exact old-generation retirement is now confirmed.
  assert(destroyPrefix(&context,&adapter)==STATUS_SUCCESS && adapter.resets==1);
  assert(deleted==1 && adapter.calls==calls);
  assert(g_VioGpuNativeImports.Flink==&g_VioGpuNativeImports);
  assert(registryHeld==0 && rundownHeld==0 && snapshotHeld==0); ++cases;
 }
 // Normal teardown sends an actual detach before deleting its record.
 {
  VioGpuDod adapter; activeAdapter=&adapter; VIOGPU_WDDM_CONTEXT context,other;
  auto *entry=new VIOGPU_WDDM_NATIVE_IMPORT_ENTRY{{},&context,&adapter,11,4096,99};
  g_VioGpuNativeImports={&entry->Link,&entry->Link};
  entry->Link={&g_VioGpuNativeImports,&g_VioGpuNativeImports};
  snapshotOK=true; snapshotGeneration=7; deleted=0;
  assert(RemoveNativeImportsForContext(&other)==STATUS_SUCCESS && deleted==0); ++cases;
  assert(RemoveNativeImportsForContext(&context)==STATUS_SUCCESS);
  assert(deleted==1 && adapter.calls==1 && adapter.acquired==adapter.released); ++cases;
 }
 std::printf("production context import cleanup: %d cases PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='native-import-cleanup-') as tmp:
    tmp = Path(tmp)
    variants = [('production', cleanup, caller), ('ignore-detach-failure', cleanup.replace(
        guard, 'if ((void)result, false)', 1), caller),
        ('ignore-caller-failure', cleanup, caller.replace('if (!NT_SUCCESS(importStatus))',
         'if ((void)importStatus, false)', 1))]
    for name, code, caller_code in variants:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text('#include <initializer_list>\n' + cleanup_fixture.replace('FUNCTION', code).replace('CALLER', caller_code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'STATUS_DEVICE_BUSY' in result.stderr, result.stderr
            print('negative control: premature context cleanup detected')

import_start = source.index('static NTSTATUS ImportNativeShareLocked(')
loop_start = source.index('    for (PLIST_ENTRY link = g_VioGpuNativeImports.Flink;', import_start)
loop_end = source.index('    VIOGPU_WDDM_NATIVE_IMPORT_ENTRY *entry = new', loop_start)
admission = source[loop_start:loop_end]
domain_fixture = revoke_fixture.split('FUNCTION')[0] + r'''
using ULONGLONG=unsigned long long;
int admission(VIOGPU_WDDM_CONTEXT *context,const VIOGPU_WDDM_NATIVE_SHARE *request,
              ULONGLONG requestEnd,ULONG *stage) {
FUNCTION
 return STATUS_SUCCESS;
}
int main() {
 VioGpuDod adapter; VIOGPU_WDDM_CONTEXT owner,child,sibling,independent;
 child.DomainOwner=sibling.DomainOwner=&owner;
 auto *entry=new VIOGPU_WDDM_NATIVE_IMPORT_ENTRY{{},&child,&adapter,11,4096,99};
 g_VioGpuNativeImports={&entry->Link,&entry->Link};
 entry->Link={&g_VioGpuNativeImports,&g_VioGpuNativeImports};
 int cases=0;
 for(auto *context: {&owner,&child,&sibling,&independent}) {
  bool shared=context!=&independent;
  ULONG stage=999; VIOGPU_WDDM_NATIVE_SHARE request{11,16384};
  int result=admission(context,&request,20479,&stage);
  assert(result==(shared?STATUS_INVALID_PARAMETER:STATUS_SUCCESS));
  assert(stage==(shared?34u:999u)); ++cases;
  request={12,6144}; stage=999;
  result=admission(context,&request,10239,&stage);
  assert(result==(shared?STATUS_INVALID_PARAMETER:STATUS_SUCCESS));
  assert(stage==(shared?35u:999u)); ++cases;
  request={12,8192}; stage=999;
  assert(admission(context,&request,12287,&stage)==STATUS_SUCCESS && stage==999); ++cases;
 }
 RemoveEntryList(&entry->Link); delete entry;
 std::printf("production native domain import admission: %d cases PASS\n",cases);
}
'''
anchor = 'NativeRegistration(existing->Context) != NativeRegistration(context)'
assert anchor in admission
with tempfile.TemporaryDirectory(prefix='native-domain-import-') as tmp:
    tmp = Path(tmp)
    for name, code in [('production', admission), ('child-only', admission.replace(
            anchor, 'existing->Context != context', 1))]:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text('#include <initializer_list>\n' + domain_fixture.replace('FUNCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'result==' in result.stderr, result.stderr
            print('negative control: cross-child duplicate import detected')
