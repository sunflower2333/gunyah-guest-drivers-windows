import fs from "node:fs";

const [inputPath, outputPath, diskPath] = process.argv.slice(2);
if (!inputPath || !outputPath || !diskPath) {
  throw new Error("usage: update-droidvm-disk-path.mjs INPUT OUTPUT DISK_PATH");
}

const config = JSON.parse(fs.readFileSync(inputPath, "utf8"));
if (!Array.isArray(config.vms) || config.vms.length !== 1) {
  throw new Error(`expected exactly one VM, found ${config.vms?.length ?? "invalid"}`);
}

const vm = config.vms[0];
if (vm.memory_mb !== 2048) {
  throw new Error(`expected memory_mb=2048, found ${vm.memory_mb}`);
}
if (vm.backend !== "crosvm" || vm.gpu_mode !== "native" || vm.gpu_provider !== "drm2kgsl") {
  throw new Error("VM is not the expected crosvm Native Context / Drm-to-Kgsl configuration");
}
if (vm.gpu_udmabuf !== true || vm.display_backend !== "virtio_gpu") {
  throw new Error("VM must retain udmabuf and virtio-gpu display");
}
if (!Array.isArray(vm.disks) || vm.disks.length !== 1 || vm.disks[0].readonly !== false) {
  throw new Error("expected one writable system disk");
}

vm.disks[0].path = diskPath;
const serialized = JSON.stringify(config, null, 4).replaceAll("/", "\\/");
fs.writeFileSync(outputPath, serialized, { flag: "wx" });
