#!/usr/bin/env python3
"""Inject detach at the production refresh snapshot/enqueue boundary."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'viogpudo/viogpudo.cpp').read_text()
start = source.index('void VioGpuAdapter::RefreshActiveScanout(void)')
end = source.index('\nvoid VioGpuAdapter::ThreadWorkRoutine', start)
body = source[start:end]
fixture = r'''
#include <cassert>
#include <cstdio>
using UINT=unsigned; using LONG=int; using BOOLEAN=bool; using KIRQL=int;
constexpr bool FALSE=false;
constexpr int STATUS_SUCCESS=0;
int mutexHeld=0,submissionHeld=0,spinHeld=0;
bool lockOK=true,injectDetach=false,detached=false;
LONG InterlockedExchange(LONG *p,LONG v) {auto old=*p;*p=v;return old;}
LONG InterlockedCompareExchange(LONG *p,LONG v,LONG c) {auto old=*p;if(old==c)*p=v;return old;}
void KeAcquireSpinLock(int*,int *old) {*old=0;++spinHeld;}
void KeReleaseSpinLock(int*,int) {
 --spinHeld;
 if(injectDetach && !mutexHeld) detached=true;
}
void KeReleaseMutex(int*,bool) {assert(mutexHeld==1);--mutexHeld;}
struct Dod {
 bool admitted=true,active=true; int events=0;
 bool AcquireNativeSubmissionOperation() {if(admitted)++submissionHeld;return admitted;}
 void ReleaseNativeSubmissionOperation() {assert(submissionHeld==1);--submissionHeld;}
 bool IsDriverActive() {return active;}
 void CountDisplayEvent(int id) {assert(id==48);++events;}
};
struct Queue {
 int transfers=0,flushes=0; bool transferOK=true,flushOK=true;
 void check() {
  assert(!spinHeld && !detached);
#if defined(VIOGPU_NATIVE_CONTEXT) && !defined(TEST_UNPROTECTED)
  assert(mutexHeld==1 && submissionHeld==1);
#endif
 }
 bool TransferToHost2D(UINT,int,UINT,UINT,int,int) {check();++transfers;return transferOK;}
 bool ResFlush(UINT,UINT,UINT,int,int,bool) {check();++flushes;return flushOK;}
};
struct VioGpuAdapter {
 LONG m_ScanoutRefreshRequested=1,m_ActiveScanoutResourceId=99;
 LONG m_ActiveScanoutWidth=32,m_ActiveScanoutHeight=32,m_ExplicitPresentResourceId=0;
 bool m_ActiveScanoutGuestBlob=false,m_ActiveScanoutNative=false,m_2DScanoutUnknown=false;
 int m_ActiveScanoutLock=0,m_2DScanoutMutex=0,reconciles=0;
 Dod *m_pVioGpuDod; Queue m_CtrlQueue;
 int WaitScanoutLifecycle() {assert(submissionHeld==1);if(!lockOK)return -1;++mutexHeld;return 0;}
 void Reconcile2DScanoutAfterResetLocked() {assert(mutexHeld==1);++reconciles;}
 void RefreshActiveScanout();
};
FUNCTION
int main() {
 int cases=0;
 for(int scenario=0;scenario<16;++scenario) {
#if defined(TEST_UNPROTECTED)
  if(scenario!=15)continue;
#endif
#if !defined(VIOGPU_NATIVE_CONTEXT)
  if(scenario==5||scenario==6||scenario==7||scenario==15)continue;
#endif
  Dod dod; VioGpuAdapter adapter;adapter.m_pVioGpuDod=&dod;
  lockOK=true;injectDetach=false;detached=false;
  auto &q=adapter.m_CtrlQueue;
  if(scenario==1)adapter.m_ActiveScanoutGuestBlob=true;
  if(scenario==2)adapter.m_ActiveScanoutNative=true;
  if(scenario==3)adapter.m_ScanoutRefreshRequested=0;
  if(scenario==4)adapter.m_pVioGpuDod=nullptr;
  if(scenario==5)dod.admitted=false;
  if(scenario==6)lockOK=false;
  if(scenario==7)adapter.m_2DScanoutUnknown=true;
  if(scenario==8)dod.active=false;
  if(scenario==9)adapter.m_ActiveScanoutResourceId=0;
  if(scenario==10)adapter.m_ActiveScanoutWidth=0;
  if(scenario==11)adapter.m_ActiveScanoutHeight=0;
  if(scenario==12)adapter.m_ExplicitPresentResourceId=99;
  if(scenario==13)q.transferOK=false;
  if(scenario==14)q.flushOK=false;
  if(scenario==15)injectDetach=true;
  adapter.RefreshActiveScanout();
  bool work=scenario<3||scenario>=13;
  assert(q.transfers==(work&&scenario!=1&&scenario!=2));
  assert(q.flushes==(work&&scenario!=13));
  assert(dod.events==(work&&scenario!=13&&scenario!=14));
  assert(!mutexHeld&&!submissionHeld&&!spinHeld&&!detached);
  ++cases;
 }
 printf("production scanout refresh lifetime: %d cases PASS\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='viogpu-refresh-lifetime-') as tmp:
    tmp = Path(tmp)
    # Removing the native-context protection reproduces the old unsafe window.
    negative = body.replace('#if defined(VIOGPU_NATIVE_CONTEXT)', '#if 0')
    assert negative != body
    for name, code, native in [('native', body, True), ('display-only', body, False),
                               ('unprotected', negative, True)]:
        cpp, exe = tmp / (name + '.cpp'), tmp / name
        cpp.write_text(fixture.replace('FUNCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined'] +
                       (['-DVIOGPU_NATIVE_CONTEXT'] if native else []) +
                       (['-DTEST_UNPROTECTED'] if name == 'unprotected' else []) +
                       [str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if name != 'unprotected':
            assert result.returncode == 0, result.stderr
            print(name + ': ' + result.stdout.strip())
        else:
            assert result.returncode != 0 and '!spinHeld && !detached' in result.stderr, result.stderr
            print('negative control: refresh accessed a detached resource')
