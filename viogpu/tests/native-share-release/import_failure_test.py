#!/usr/bin/env python3
"""Exercise production import outcomes and reservation ownership with faults."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
adapter = (root / 'viogpudo/viogpudo.cpp').read_text()
ddi = (root / 'viogpuwddm/wddmddi.cpp').read_text()
start = adapter.index('VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::ImportNativeSharedResource(')
end = adapter.index('\nVIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::ReleaseNativeSharedResource(', start)
host = adapter[start:end]
start = ddi.index('static NTSTATUS ImportNativeShareLocked(')
start = ddi.index('    if (!adapter->AcquireNativeSubmissionOperation())', start)
end = ddi.index('\nstatic NTSTATUS ReleaseNativeShareLocked(', start)
tail = ddi[start:end]
prefix = r'''
#include <cassert>
#include <cstdio>
#include <initializer_list>
#define _In_
#define PAGED_CODE() ((void)0)
using UINT=unsigned; using ULONG=unsigned; using ULONGLONG=unsigned long long;
using NTSTATUS=int;
constexpr int STATUS_SUCCESS=0, STATUS_DEVICE_NOT_READY=-1;
constexpr UINT VIOGPU_NATIVE_RESOURCE_ID_START=90, MAXUINT=~0u, PAGE_SIZE=4096;
constexpr int PASSIVE_LEVEL=0, MSM_CCMD_GEM_SET_IOVA=5;
constexpr bool TRUE=true,FALSE=false;
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted,
 VioGpuHostContextConfirmed,VioGpuHostContextRejected,VioGpuHostContextUnknown };
enum { VioGpuNativeQuarantineImportAttach=1,VioGpuNativeQuarantineImportSubmit,
 VioGpuNativeQuarantineImportRollback };
struct Owner { int quarantine=0; };
void VioGpuQuarantineNativeContextOwner(Owner *o,int site) { o->quarantine=site; }
int KeGetCurrentIrql() { return PASSIVE_LEVEL; }
struct MSM_CCMD_GEM_SET_IOVA_REQ {
 struct { UINT cmd,len; } hdr; ULONGLONG iova; UINT res_id;
};
struct Queue {
 VIOGPU_HOST_CONTEXT_RESULT attach,submit,detach;
 int attaches=0,submits=0,detaches=0;
 VIOGPU_HOST_CONTEXT_RESULT SetNativeResourceAttachment(UINT,UINT,bool add) {
  if(add) {++attaches;return attach;} ++detaches;return detach;
 }
 VIOGPU_HOST_CONTEXT_RESULT SubmitNativeControl(UINT,void*,unsigned) {++submits;return submit;}
};
struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT;
struct VioGpuAdapter {
 Queue m_CtrlQueue; bool admitted=true; int acquired=0,released=0;
 bool AcquireNativeSubmissionOperation() {if(admitted) ++acquired;return admitted;}
 void ReleaseNativeSubmissionOperation() {++released;}
 bool IsNativeContextGenerationCurrent(unsigned,unsigned) {return true;}
 VIOGPU_HOST_CONTEXT_RESULT ImportNativeSharedResource(const VIOGPU_NATIVE_CONTEXT_SNAPSHOT*,UINT,ULONGLONG);
};
struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT {
 VioGpuAdapter *Adapter; struct Owner *Owner; UINT ContextId=2,Generation=1,ResetGeneration=7;
};
bool IsLiveNativeImporter(VioGpuAdapter*,const VIOGPU_NATIVE_CONTEXT_SNAPSHOT*) {return true;}
bool VioGpuNativeControlFaultsClear(VioGpuAdapter*,Owner*) {return true;}
struct Entry;
struct Link { Entry *entry; };
int destroyed=0;
struct Entry { struct Link Link{this}; ~Entry() {++destroyed;} };
Entry *retained=nullptr;
int g_VioGpuNativeImports;
void InsertTailList(int*,Link *link) {assert(!retained);retained=link->entry;}
struct Share { UINT ResourceId=99; };
struct Request { ULONGLONG Iova=4096; };
'''
wrapper = r'''
int call(VioGpuAdapter *adapter,const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *snapshot,
         ULONG *stage,ULONG *hostResult) {
 auto *entry=new Entry;
 Share storage; auto *share=&storage;
 Request req; auto *request=&req;
'''
main = r'''
int main() {
 int cases=0;
 for(int a=0;a<4;++a) for(int s=0;s<4;++s) for(int d=0;d<4;++d) {
  VioGpuAdapter adapter; Owner owner;
  auto &q=adapter.m_CtrlQueue;
  q.attach=static_cast<VIOGPU_HOST_CONTEXT_RESULT>(a);
  q.submit=static_cast<VIOGPU_HOST_CONTEXT_RESULT>(s);
  q.detach=static_cast<VIOGPU_HOST_CONTEXT_RESULT>(d);
  VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot{&adapter,&owner};
  ULONG stage=999,hostResult=999; destroyed=0;
  int status=call(&adapter,&snapshot,&stage,&hostResult);
  bool attached=a==VioGpuHostContextConfirmed;
  bool mapped=attached && s==VioGpuHostContextConfirmed;
  bool rollback=attached && s!=VioGpuHostContextConfirmed && s!=VioGpuHostContextUnknown;
  bool uncertain=a==VioGpuHostContextUnknown || (attached && s==VioGpuHostContextUnknown) ||
                 (rollback && d!=VioGpuHostContextConfirmed);
  assert((retained!=nullptr)==(mapped||uncertain));
  assert(destroyed==(mapped||uncertain?0:1));
  assert(status==(mapped?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY));
  assert(stage==(mapped?999u:38u));
  assert(q.attaches==1 && q.submits==attached && q.detaches==rollback);
  assert(adapter.acquired==1 && adapter.released==1);
  assert((owner.quarantine!=0)==uncertain);
  if(uncertain) assert(hostResult==VioGpuHostContextUnknown);
  delete retained; retained=nullptr; ++cases;
 }
 VioGpuAdapter adapter; adapter.admitted=false; Owner owner;
 VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot{&adapter,&owner};
 ULONG stage=999,hostResult=999; destroyed=0;
 assert(call(&adapter,&snapshot,&stage,&hostResult)==STATUS_DEVICE_NOT_READY);
 assert(stage==37 && hostResult==999 && destroyed==1 && !retained);
 assert(!adapter.m_CtrlQueue.attaches && !adapter.acquired && !adapter.released);
 printf("native import failure ownership: %d cases PASS\n",++cases);
}
'''
lost_record = tail.replace('InsertTailList(&g_VioGpuNativeImports, &entry->Link);', 'delete entry;', 1)
lost_rollback = host.replace('if (rollback != VioGpuHostContextConfirmed)',
                            'if (rollback == VioGpuHostContextUnknown)')
assert lost_record != tail and lost_rollback != host
with tempfile.TemporaryDirectory(prefix='viogpu-import-failure-') as tmp:
    tmp = Path(tmp)
    for name, h, t in [('production', host, tail), ('lost-record', host, lost_record),
                       ('lost-rollback', lost_rollback, tail)]:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text(prefix + h + wrapper + t + main)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'retained!=nullptr' in result.stderr, result.stderr
            print('negative control detected:', name)
