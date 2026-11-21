'use strict';

const { createHash } = require('node:crypto');
const fs = require('node:fs/promises');
const path = require('node:path');

/** PNG signature bytes, as defined by the PNG specification. */
const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

/** Extensions the Windows conversion helper can turn into PNG. */
const CONVERTIBLE_EXTENSIONS = new Set(['.jpg', '.jpeg', '.bmp', '.gif']);

/** Upper bound for a converted cover, keeping the private profile small. */
const MAX_SOURCE_BYTES = 32 * 1024 * 1024;

function isPngHeader(header) {
  return Buffer.isBuffer(header) && header.length >= PNG_SIGNATURE.length &&
    header.subarray(0, PNG_SIGNATURE.length).equals(PNG_SIGNATURE);
}

async function readHeader(file, length = PNG_SIGNATURE.length) {
  const handle = await fs.open(file, 'r');
  try {
    const buffer = Buffer.alloc(length);
    const { bytesRead } = await handle.read(buffer, 0, length, 0);
    return buffer.subarray(0, bytesRead);
  } finally {
    await handle.close();
  }
}

function quote(value) {
  return `'${String(value).replaceAll("'", "''")}'`;
}

function defaultPowerShell() {
  return path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
}

/**
 * Create a converter that rewrites a Windows-supported image as PNG.
 *
 * @param {object} options Conversion dependencies.
 * @param {Function} options.execFile Promisified `child_process.execFile`.
 * @param {string} [options.powershell] PowerShell executable to run.
 * @returns {Function} Converter accepting a source and destination path.
 */
function createWindowsPngConverter({execFile, powershell = defaultPowerShell()} = {}) {
  if (typeof execFile !== 'function') throw new TypeError('A process runner is required to convert cover images.');
  return async function convert(source, destination) {
    const script = [
      'Add-Type -AssemblyName System.Drawing',
      `$image = [System.Drawing.Image]::FromFile(${quote(source)})`,
      `try { $image.Save(${quote(destination)}, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $image.Dispose() }`
    ].join('; ');
    await execFile(powershell, ['-NoProfile', '-NonInteractive', '-Command', script], {windowsHide: true, timeout: 30000});
  };
}

async function exists(file) {
  try {
    await fs.access(file);
    return true;
  } catch {
    return false;
  }
}

/**
 * Resolve a user-chosen image into a cover PNG the host engine accepts.
 *
 * A valid PNG is referenced where it already lives. Other supported formats are
 * copied into the private covers directory as PNG, which keeps the engine's
 * extension and signature checks satisfied without rewriting the source file.
 *
 * @param {object} options Cover request.
 * @param {string} options.file Image chosen by the user.
 * @param {string} options.coversDirectory Directory receiving converted covers.
 * @param {Function} [options.convert] Converter used for non-PNG input.
 * @param {number} [options.maxBytes] Source size limit before conversion.
 * @returns {Promise<string>} Absolute PNG path to store in `image-path`.
 */
async function ensurePngCover({file, coversDirectory, convert, maxBytes = MAX_SOURCE_BYTES}) {
  const source = path.resolve(file);
  let header;
  try {
    header = await readHeader(source);
  } catch (error) {
    if (error.code === 'ENOENT') throw new Error('找不到所选封面图片。');
    throw error;
  }
  if (isPngHeader(header)) return source;

  const extension = path.extname(source).toLowerCase();
  if (extension === '.png') throw new Error('所选文件不是有效的 PNG 图片。');
  if (!CONVERTIBLE_EXTENSIONS.has(extension)) throw new Error('封面仅支持 PNG、JPG、BMP 或 GIF 图片。');
  if (typeof convert !== 'function') throw new Error('当前环境无法把图片转换为 PNG。');

  const {size} = await fs.stat(source);
  const limitMb = Math.max(1, Math.round(maxBytes / 1024 / 1024));
  if (size > maxBytes) throw new Error(`封面图片不能大于 ${limitMb} MB。`);

  const data = await fs.readFile(source);
  const name = `cover-${createHash('sha256').update(data).digest('hex').slice(0, 16)}.png`;
  await fs.mkdir(coversDirectory, {recursive: true});
  const destination = path.join(coversDirectory, name);
  if (await exists(destination)) return destination;

  try {
    await convert(source, destination);
  } catch (error) {
    await fs.rm(destination, {force: true});
    throw new Error('无法把所选图片转换为 PNG。', {cause: error});
  }
  if (!isPngHeader(await readHeader(destination).catch(() => Buffer.alloc(0)))) {
    await fs.rm(destination, {force: true});
    throw new Error('无法把所选图片转换为 PNG。');
  }
  return destination;
}

module.exports = {PNG_SIGNATURE, createWindowsPngConverter, ensurePngCover, isPngHeader};
