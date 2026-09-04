import { randomUUID } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';

const [sourcePath, outputPath, namePrefix, barSize] = process.argv.slice(2);
if (!sourcePath || !outputPath || !namePrefix || !barSize) {
  throw new Error(
    'usage: node prepare-bar-prebacked-config.mjs SOURCE OUTPUT NAME_PREFIX BAR_SIZE',
  );
}

const config = JSON.parse(readFileSync(sourcePath, 'utf8'));
const id = randomUUID();
config.id = id;
config.name = `${namePrefix}-${id.slice(0, 8)}`;
config.pid = -1;
config.state = 'STOPPED';

if (barSize === 'default') {
  delete config.gpu_pci_bar_size;
} else {
  const parsedBarSize = Number(barSize);
  if (!Number.isSafeInteger(parsedBarSize) || parsedBarSize <= 0) {
    throw new Error(`invalid BAR size: ${barSize}`);
  }
  config.gpu_pci_bar_size = parsedBarSize;
}

writeFileSync(outputPath, `${JSON.stringify(config)}\n`, { mode: 0o600 });
process.stdout.write(`${JSON.stringify({ id, name: config.name, outputPath })}\n`);
