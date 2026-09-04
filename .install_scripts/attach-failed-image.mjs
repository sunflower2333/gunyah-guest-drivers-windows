import fs from 'node:fs';

const [inputPath, outputPath, vmId, overlayPath] = process.argv.slice(2);
if (!inputPath || !outputPath || !vmId || !overlayPath) {
  throw new Error('usage: node attach-failed-image.mjs <input> <output> <vm-id> <overlay-path>');
}

const config = JSON.parse(fs.readFileSync(inputPath, 'utf8'));
const matches = config.vms.filter((vm) => vm.id === vmId);
if (matches.length !== 1) {
  throw new Error(`expected exactly one VM ${vmId}, found ${matches.length}`);
}

const vm = matches[0];
const existing = vm.disks.filter((disk) => disk.path === overlayPath);
if (existing.length > 1) {
  throw new Error(`duplicate overlay entries for ${overlayPath}`);
}
if (existing.length === 0) {
  vm.disks.push({
    bus: 'virtio',
    path: overlayPath,
    readonly: false,
  });
}

fs.writeFileSync(outputPath, `${JSON.stringify(config, null, 4)}\n`, { flag: 'wx' });
