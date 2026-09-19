#!/usr/bin/env python3
"""Inject host failures/reset into production standard/native scanout detach."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'viogpudo/viogpudo.cpp').read_text()
start = source.index('VIOGPU_HOST_CONTEXT_RESULT VioGpuAdapter::Detach2DScanoutResource(')
body = source[start:source.index('\nBOOLEAN VioGpuAdapter::Query2DScanoutResource(', start)]
fixture = r'''
#include <cassert>
#include <cstdio>
#include <initializer_list>
#define _In_
#define _Out_
using UINT=unsigned;using BOOLEAN=bool;using ULONGLONG=unsigned long long;
using NTSTATUS=int;
constexpr bool TRUE=true,FALSE=false;
constexpr unsigned MAXUINT=~0u,VIOGPU_NATIVE_RESOURCE_ID_START=100;
constexpr int PASSIVE_LEVEL=0,STATUS_SUCCESS=0,VioGpuNativeFailSiteScanout2DDetach=1;
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted,VioGpuHostContextConfirmed,
 VioGpuHostContextRejected,VioGpuHostContextUnknown };
int irql=0,held=0,waitStatus=0;
int KeGetCurrentIrql() {return irql;}
ULONGLONG InterlockedCompareExchange64(ULONGLONG *v,int,int) {return *v;}
void KeReleaseMutex(int*,bool) {assert(held==1);--held;}
struct Queue {
 int calls=0;VIOGPU_HOST_CONTEXT_RESULT result=VioGpuHostContextConfirmed;
 VIOGPU_HOST_CONTEXT_RESULT SetScanoutSynchronous(int,int,int,int,int,int) {
  assert(held==1);++calls;return result;
 }
};
struct VioGpuAdapter {
 UINT m_2DScanoutResourceId=0,m_PublishedScanoutResourceId=0;
 ULONGLONG m_NativeContextResetGeneration=7,m_2DScanoutResetGeneration=7;
 bool m_2DScanoutUnknown=false,retired=false;int m_2DScanoutMutex=0,faults=0;
 Queue m_CtrlQueue;
 int WaitScanoutLifecycle() {if(!waitStatus)++held;return waitStatus;}
 void Reconcile2DScanoutAfterResetLocked() {
  assert(held==1);if(retired){m_2DScanoutResourceId=0;m_2DScanoutUnknown=false;}
 }
 void RecordActiveScanout(int,int,int) {assert(held==1);}
 void FailNativeContextAtAnyIrql(int) {++faults;}
 VIOGPU_HOST_CONTEXT_RESULT Detach2DScanoutResource(UINT,BOOLEAN*,BOOLEAN);
};
FUNCTION
int main() {
 int cases=0;
 for(bool native: {false,true}) for(int scenario=0;scenario<13;++scenario) {
  VioGpuAdapter adapter;UINT id=native?101:1;
  adapter.m_2DScanoutResourceId=id;adapter.m_PublishedScanoutResourceId=id;
  irql=waitStatus=held=0;bool detached=false;
  if(scenario==1)waitStatus=258;
  if(scenario==2)adapter.m_2DScanoutUnknown=true;
  if(scenario==3)adapter.m_NativeContextResetGeneration=0;
  if(scenario==4)adapter.m_CtrlQueue.result=VioGpuHostContextUnknown;
  if(scenario==5)adapter.m_CtrlQueue.result=VioGpuHostContextRejected;
  if(scenario==6)adapter.m_CtrlQueue.result=VioGpuHostContextNotSubmitted;
  if(scenario==7)adapter.m_2DScanoutResourceId=55;
  if(scenario==8){adapter.m_2DScanoutUnknown=true;adapter.retired=true;}
  if(scenario==9)irql=2;
  if(scenario==10)id=0;
  if(scenario==11)id=MAXUINT;
  if(scenario==12)id=native?1:101;
  auto result=adapter.Detach2DScanoutResource(id,&detached,native);
  assert(!held);
  bool success=scenario==0||scenario==7||scenario==8;
  assert(detached==success);
  assert((result==VioGpuHostContextConfirmed)==success);
  assert(adapter.m_CtrlQueue.calls==(scenario==0||scenario==4||scenario==5||scenario==6));
  if(scenario==0)assert(adapter.m_2DScanoutResourceId==0&&adapter.m_2DScanoutResetGeneration==0);
  if(scenario==4)assert(adapter.m_2DScanoutUnknown&&adapter.faults==1);
  if(scenario>=4&&scenario<=6)assert(adapter.m_2DScanoutResourceId==(native?101u:1u));
  if(scenario==7)assert(adapter.m_2DScanoutResourceId==55); // Do not unbind a replacement.
  ++cases;
 }
 printf("production standard/native scanout detach: %d cases PASS\n",cases);
}
'''
negative = body.replace('if (result == VioGpuHostContextConfirmed)', 'if (true)', 1)
with tempfile.TemporaryDirectory(prefix='viogpu-native-detach-') as directory:
    temp = Path(directory)
    for name, code in [('production', body), ('unconfirmed-detach', negative)]:
        cpp, exe = temp / (name + '.cpp'), temp / name
        cpp.write_text(fixture.replace('FUNCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name == 'production':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'detached==success' in result.stderr, result.stderr
            print('negative control detected: unconfirmed scanout detach')
