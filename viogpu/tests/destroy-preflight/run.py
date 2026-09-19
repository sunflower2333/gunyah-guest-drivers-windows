#!/usr/bin/env python3
"""Execute the production destroy preflight; reject mutation on failed requests."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'viogpuwddm/wddmddi.cpp').read_text()
start = source.index('_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmDestroyAllocation(')
end = source.index('        VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot = {};', start)
end = source.rfind('    for (UINT index', start, end)
prefix = source[start:end]
side_start = prefix.index('    /* Other contexts may still map')
side = prefix[side_start:]
assert 'RevokeNativeShares(' in side and 'CancelPendingFlip(' in side
resource_start = prefix.index('    VIOGPU_WDDM_RESOURCE *resource = NULL;')
assert resource_start < side_start
negative = prefix[:resource_start] + side + prefix[resource_start:side_start]
fixture = r'''
#include <cstdio>
#include <cstddef>
using UINT = unsigned; using LONG = long; using HANDLE = void*; using NTSTATUS = int;
#define _Use_decl_annotations_
#define APIENTRY
#define CONST const
constexpr int PASSIVE_LEVEL=0, STATUS_SUCCESS=0, STATUS_INVALID_PARAMETER=-1,
 STATUS_INVALID_HANDLE=-2, STATUS_DEVICE_BUSY=-3;
#define NT_SUCCESS(x) ((x)>=0)
constexpr int VIOGPU_WDDM_ALLOCATION_SIGNATURE=123, VIOGPU_WDDM_RESOURCE_SIGNATURE=456;
struct VIOGPU_WDDM_ALLOCATION;
struct VioGpuDod { int revoked=0, cancelled=0; void CancelPendingFlip(VIOGPU_WDDM_ALLOCATION*) { ++cancelled; } };
struct VIOGPU_WDDM_RESOURCE { int Signature=456; VioGpuDod *Adapter; LONG count; };
struct VIOGPU_WDDM_ALLOCATION {
 int Signature=123; VioGpuDod *Adapter; VIOGPU_WDDM_RESOURCE *Resource;
 UINT ResourceId; bool native, primary, owned=true;
};
struct DXGKARG_DESTROYALLOCATION {
 UINT NumAllocations; HANDLE *pAllocationList; HANDLE hResource;
 union { UINT Value; struct { UINT DestroyResource:1; }; } Flags;
};
int GetCurrentIrql=0;
int KeGetCurrentIrql() { return GetCurrentIrql; }
bool IsOwnedAllocation(VIOGPU_WDDM_ALLOCATION *a,VioGpuDod*) { return a->owned; }
bool IsNativeAllocation(VIOGPU_WDDM_ALLOCATION *a) { return a->native; }
bool IsStandardPrimaryAllocation(VIOGPU_WDDM_ALLOCATION *a) { return a->primary; }
LONG ReadResourceAllocationCount(VIOGPU_WDDM_RESOURCE *r) { return r->count; }
int revokeResult=0;
int closeResult=0;
int CloseNativeAllocationExports(VIOGPU_WDDM_ALLOCATION*) { return closeResult; }
int RevokeNativeShares(VioGpuDod *a,UINT) { ++a->revoked; return revokeResult; }
// PRODUCTION
    return STATUS_SUCCESS; // Stop at the real lifecycle boundary; no simulated teardown.
}
int failures=0, cases=0;
void check(const char *name,VioGpuDod &a,DXGKARG_DESTROYALLOCATION &arg,int status,int revoke,int cancel) {
 a.revoked=a.cancelled=0;
 int got=VioGpuWddmDestroyAllocation(&a,&arg); ++cases;
 if(got!=status || a.revoked!=revoke || a.cancelled!=cancel) {
  std::printf("FAIL %s status=%d revoke=%d cancel=%d\n",name,got,a.revoked,a.cancelled); ++failures;
 }
}
int main() {
 VioGpuDod a,other;
 VIOGPU_WDDM_RESOURCE r{456,&a,2}, wrong{456,&a,2};
 VIOGPU_WDDM_ALLOCATION n{123,&a,&r,100,true,false}, p{123,&a,&r,101,false,true};
 HANDLE list[]={&n,&p}; DXGKARG_DESTROYALLOCATION arg{2,list,&r,{1}};
 check("valid-resource",a,arg,0,1,1);
 closeResult=STATUS_DEVICE_BUSY;
 check("busy-export-retirement",a,arg,STATUS_DEVICE_BUSY,0,0);
 closeResult=0;
 revokeResult=STATUS_DEVICE_BUSY;
 check("unconfirmed-revoke",a,arg,STATUS_DEVICE_BUSY,1,0);
 revokeResult=0;
 arg.hResource=nullptr; check("null-resource",a,arg,-2,0,0); arg.hResource=&r;
 r.Signature=0; check("bad-resource-signature",a,arg,-2,0,0); r.Signature=456;
 r.Adapter=&other; check("foreign-resource",a,arg,-2,0,0); r.Adapter=&a;
 r.count=-1; check("negative-count",a,arg,-2,0,0);
 r.count=3; check("busy-incomplete-resource",a,arg,-3,0,0); r.count=2;
 arg.hResource=&wrong; check("wrong-membership",a,arg,-2,0,0); arg.hResource=&r;
 list[1]=&n; check("duplicate-allocation",a,arg,-1,0,0); list[1]=&p;
 p.Signature=0; check("bad-second-allocation",a,arg,-2,0,0); p.Signature=123;
 p.owned=false; check("unowned-allocation",a,arg,-2,0,0); p.owned=true;
 arg.Flags.Value=2; check("invalid-flags",a,arg,-1,0,0); arg.Flags.Value=1;
 arg.pAllocationList=nullptr; check("missing-list",a,arg,-1,0,0); arg.pAllocationList=list;
 GetCurrentIrql=1; check("invalid-irql",a,arg,-1,0,0); GetCurrentIrql=0;
 arg.Flags.Value=0; arg.hResource=nullptr; check("allocation-only",a,arg,0,1,1);
 arg.NumAllocations=0; check("empty-list",a,arg,0,0,0);
 std::printf("cases=%d failures=%d\n",cases,failures); return failures?1:0;
}
'''
for label, code in [('current', prefix), ('old-early-side-effects', negative)]:
    with tempfile.TemporaryDirectory(prefix='destroy-preflight-') as temp:
        path = Path(temp)
        (path / 'test.cpp').write_text(fixture.replace('// PRODUCTION', code))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(path / 'test.cpp'),
                        '-o', str(path / 'test')], check=True)
        result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
        print(label, result.returncode, result.stdout, result.stderr)
        assert result.returncode == (0 if label == 'current' else 1)
        if label != 'current':
            assert 'FAIL busy-incomplete-resource' in result.stdout
            assert 'FAIL wrong-membership' in result.stdout
