[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

# Drive the *system* Vulkan loader (vulkan-1.dll) rather than loading the ICD
# directly, so this proves the registered ICD is reachable the way vkcube sees it.
Add-Type -Namespace Vk -Name Api -MemberDefinition @'
[DllImport("vulkan-1.dll", CallingConvention = CallingConvention.Winapi)]
public static extern int vkCreateInstance(byte[] createInfo, IntPtr alloc, out IntPtr instance);
[DllImport("vulkan-1.dll", CallingConvention = CallingConvention.Winapi)]
public static extern int vkEnumeratePhysicalDevices(IntPtr instance, ref uint count, IntPtr[] devices);
[DllImport("vulkan-1.dll", CallingConvention = CallingConvention.Winapi)]
public static extern void vkGetPhysicalDeviceProperties(IntPtr device, byte[] props);
[DllImport("vulkan-1.dll", CallingConvention = CallingConvention.Winapi)]
public static extern void vkDestroyInstance(IntPtr instance, IntPtr alloc);
'@

# VkInstanceCreateInfo: sType(4) pad(4) pNext(8) flags(4) pad(4) pAppInfo(8)
# enabledLayerCount(4) pad(4) ppLayers(8) enabledExtCount(4) pad(4) ppExts(8) = 64 bytes
$ci = New-Object byte[] 64
[BitConverter]::GetBytes([uint32]1).CopyTo($ci, 0)   # VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO

$instance = [IntPtr]::Zero
$r = [Vk.Api]::vkCreateInstance($ci, [IntPtr]::Zero, [ref]$instance)
Write-Output "vkCreateInstance = $r"
if ($r -ne 0) { Write-Output "RESULT=FAIL (instance)"; exit 1 }

[uint32]$count = 0
$r = [Vk.Api]::vkEnumeratePhysicalDevices($instance, [ref]$count, $null)
Write-Output "vkEnumeratePhysicalDevices(count) = $r count=$count"
if ($r -ne 0 -or $count -eq 0) {
    [Vk.Api]::vkDestroyInstance($instance, [IntPtr]::Zero)
    Write-Output "RESULT=FAIL (no physical devices)"; exit 1
}

$devs = New-Object IntPtr[] $count
$r = [Vk.Api]::vkEnumeratePhysicalDevices($instance, [ref]$count, $devs)
Write-Output "vkEnumeratePhysicalDevices(fill) = $r"

for ($i = 0; $i -lt $count; $i++) {
    $props = New-Object byte[] 1024
    [Vk.Api]::vkGetPhysicalDeviceProperties($devs[$i], $props)
    $api = [BitConverter]::ToUInt32($props, 0)
    $drv = [BitConverter]::ToUInt32($props, 4)
    $vendor = [BitConverter]::ToUInt32($props, 8)
    $devid = [BitConverter]::ToUInt32($props, 12)
    $type = [BitConverter]::ToUInt32($props, 16)
    $name = [Text.Encoding]::ASCII.GetString($props, 20, 256).TrimEnd([char]0)
    $typeName = @('OTHER','INTEGRATED_GPU','DISCRETE_GPU','VIRTUAL_GPU','CPU')[$type]
    "device[$i]: $name"
    "   apiVersion={0}.{1}.{2} driverVersion=0x{3:X} vendorID=0x{4:X} deviceID=0x{5:X} type=$typeName" -f `
        (($api -shr 22) -band 0x7F), (($api -shr 12) -band 0x3FF), ($api -band 0xFFF), $drv, $vendor, $devid
}

[Vk.Api]::vkDestroyInstance($instance, [IntPtr]::Zero)
Write-Output "RESULT=PASS"
