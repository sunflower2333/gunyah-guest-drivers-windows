#!/usr/bin/env python3
"""Exercise production export admission across destruction and failed retries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'viogpuwddm/wddmddi.cpp').read_text()
def extract(start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]

begin = extract('NTSTATUS BeginAllocationDestroy(', '// Close export admission')
close = extract('static NTSTATUS CloseNativeAllocationExports(', 'NTSTATUS AcquireRenderAllocationReferences(')
lookup = extract('static VOID FindNativeAllocationRangeByIova(', '/* Runs under the context')
destroy = extract('_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDestroyAllocation(',
                  '_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDescribeAllocation(')
assert destroy.index('CloseNativeAllocationExports(') < destroy.index('RevokeNativeShares(')
register = extract('NTSTATUS RegisterNativeAllocationRange(', 'NTSTATUS UnregisterNativeAllocationRange(')
assert 'allocation->Destroying' in register.split('VIOGPU_WDDM_ALLOCATION_RANGE *range = new')[0]
fixture = r'''
#include <cassert>
#include <cstddef>
#include <cstdio>
using NTSTATUS=int; using UINT=unsigned; using ULONGLONG=unsigned long long;
using VOID=void; using KIRQL=int; using BOOLEAN=bool;
#define _In_
#define _Out_
constexpr bool TRUE=true,FALSE=false;
constexpr int STATUS_SUCCESS=0,STATUS_DEVICE_BUSY=-1,STATUS_DEVICE_NOT_READY=-2,
 STATUS_INVALID_PARAMETER=-3,STATUS_INVALID_HANDLE=-4,STATUS_GRAPHICS_ALLOCATION_BUSY=-5;
constexpr int VIOGPU_WDDM_ALLOCATION_SIGNATURE=123;
struct LIST_ENTRY { LIST_ENTRY *Flink,*Blink; };
using PLIST_ENTRY=LIST_ENTRY*;
#define CONTAINING_RECORD(p,t,f) reinterpret_cast<t*>(reinterpret_cast<char*>(p)-offsetof(t,f))
struct VIOGPU_NATIVE_CONTEXT_REGISTRATION { int BindingLock=1; LIST_ENTRY AllocationRanges; };
struct VIOGPU_WDDM_ALLOCATION_RANGE {
 LIST_ENTRY Link; VIOGPU_NATIVE_CONTEXT_REGISTRATION *Registration;
 ULONGLONG Iova=4096,Length=8192; UINT ResourceId=99,ContextId=7;
 bool Linked=true,ExportRetired=false;
};
struct VIOGPU_WDDM_ALLOCATION {
 int Signature=123,SubmissionLock=2,LifecycleMutex=3;
 int SubmissionReferences=0,OpenReferences=0; bool Destroying=false;
 VIOGPU_NATIVE_CONTEXT_REGISTRATION *NativeContext;
 VIOGPU_WDDM_ALLOCATION_RANGE *ContextRange;
};
struct VIOGPU_WDDM_CONTEXT { VIOGPU_NATIVE_CONTEXT_REGISTRATION *registration; };
VIOGPU_NATIVE_CONTEXT_REGISTRATION *NativeRegistration(VIOGPU_WDDM_CONTEXT *c) {return c->registration;}
int spin=0,lifecycle=0,waitStatus=0; bool valid=true;
void KeAcquireSpinLock(int*,int *old) {assert(!spin);++spin;*old=0;}
void KeReleaseSpinLock(int*,int) {assert(spin==1);--spin;}
void KeReleaseMutex(int*,bool) {assert(lifecycle==1);--lifecycle;}
int AcquireAllocationLifecycleForDestroy(VIOGPU_WDDM_ALLOCATION*) {
 if(waitStatus==0) {assert(!lifecycle);++lifecycle;} return waitStatus;
}
bool ValidateNativeAllocationDestroyState(VIOGPU_WDDM_ALLOCATION*) {assert(lifecycle==1);return valid;}
BEGIN
CLOSE
LOOKUP
int main() {
 int cases=0;
 for(int scenario=0;scenario<10;++scenario) {
  VIOGPU_NATIVE_CONTEXT_REGISTRATION registration,foreign;
  VIOGPU_WDDM_ALLOCATION_RANGE range{{},&registration};
  registration.AllocationRanges={&range.Link,&range.Link};
  range.Link={&registration.AllocationRanges,&registration.AllocationRanges};
  VIOGPU_WDDM_CONTEXT context{&registration};
  VIOGPU_WDDM_ALLOCATION allocation;allocation.NativeContext=&registration;allocation.ContextRange=&range;
  auto visible=[&]() {UINT id=0;ULONGLONG size=0;
   FindNativeAllocationRangeByIova(&context,4096,7,&id,&size);
   assert((id==99&&size==8192)||(id==0&&size==0));return id!=0;
  };
  valid=true;waitStatus=0;assert(visible());
  if(scenario==1)waitStatus=258; // STATUS_TIMEOUT is nonnegative, not success.
  if(scenario==2)valid=false;
  if(scenario==3)allocation.SubmissionReferences=1;
  if(scenario==4)allocation.OpenReferences=1;
  if(scenario==5)range.Linked=false;
  if(scenario==6)range.Registration=&foreign;
  if(scenario==7)allocation.ContextRange=nullptr;
  if(scenario==8)allocation.NativeContext=nullptr; // Already-detached retry.
  if(scenario==9)allocation.Signature=0;
  int result=CloseNativeAllocationExports(&allocation);
  assert(!spin&&!lifecycle);
  if(scenario==0) {
   assert(result==0 && allocation.Destroying && range.ExportRetired && !visible());
   // Simulate failed importer/host release. The range stays linked until
   // confirmed teardown, but a retry must not reopen export admission.
   for(int retry=0;retry<3;++retry) {
    assert(CloseNativeAllocationExports(&allocation)==0);
    assert(range.Linked && !visible() && !spin && !lifecycle);++cases;
   }
  } else if(scenario==8) {
   assert(result==0 && allocation.Destroying);
  } else {
   assert(result!=0 && !range.ExportRetired);
   if(scenario==1)assert(result==STATUS_DEVICE_BUSY && !allocation.Destroying);
   if(scenario==3||scenario==4) {
    assert(allocation.Destroying && result==STATUS_GRAPHICS_ALLOCATION_BUSY);
    allocation.SubmissionReferences=allocation.OpenReferences=0;
    assert(CloseNativeAllocationExports(&allocation)==0 && !visible());++cases;
   }
  }
  ++cases;
 }
 printf("production export retirement: %d cases PASS\n",cases);
}
'''
assert 'range->ExportRetired = TRUE;' in close
assert '!range->ExportRetired &&' in lookup
variants = [('production', close, lookup),
            ('reopened-export', close.replace('range->ExportRetired = TRUE;', 'range->ExportRetired = FALSE;'), lookup),
            ('ignored-retirement', close, lookup.replace('!range->ExportRetired && ', ''))]
with tempfile.TemporaryDirectory(prefix='viogpu-export-retirement-') as directory:
    temp = Path(directory)
    for name, close_code, lookup_code in variants:
        cpp, exe = temp / (name + '.cpp'), temp / name
        cpp.write_text(fixture.replace('BEGIN', begin).replace('CLOSE', close_code).replace('LOOKUP', lookup_code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and '!visible()' in result.stderr, result.stderr
            print('negative control detected: ' + name)
