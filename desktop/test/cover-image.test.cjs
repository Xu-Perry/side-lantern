'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const {PNG_SIGNATURE, createWindowsPngConverter, ensurePngCover, isPngHeader} = require('../lib/cover-image.cjs');

const pngBytes = Buffer.concat([PNG_SIGNATURE, Buffer.from([0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52])]);
const jpegBytes = Buffer.from([0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46]);

async function temporaryDirectory() {
  return fs.mkdtemp(path.join(os.tmpdir(), 'side-lantern-cover-'));
}

test('the PNG signature is recognised only from its exact leading bytes', () => {
  assert.equal(isPngHeader(pngBytes), true);
  assert.equal(isPngHeader(Buffer.from([0x89, 0x50, 0x4e, 0x47])), false);
  assert.equal(isPngHeader(Buffer.from('not a png at all')), false);
  assert.equal(isPngHeader('not a buffer'), false);
});

test('a valid PNG is referenced where the user keeps it', async () => {
  const directory = await temporaryDirectory();
  const file = path.join(directory, 'Poster.PNG');
  await fs.writeFile(file, pngBytes);

  const cover = await ensurePngCover({file, coversDirectory: path.join(directory, 'covers')});

  assert.equal(cover, file);
  await assert.rejects(fs.access(path.join(directory, 'covers')), {code: 'ENOENT'});
});

test('a missing file reports a readable error', async () => {
  const directory = await temporaryDirectory();
  await assert.rejects(
    ensurePngCover({file: path.join(directory, 'absent.png'), coversDirectory: directory}),
    /找不到所选封面图片/
  );
});

test('a .png file that is not really a PNG is rejected', async () => {
  const directory = await temporaryDirectory();
  const file = path.join(directory, 'renamed.png');
  await fs.writeFile(file, jpegBytes);

  await assert.rejects(ensurePngCover({file, coversDirectory: path.join(directory, 'covers')}), /不是有效的 PNG/);
});

test('a JPEG is converted into the private covers directory', async () => {
  const directory = await temporaryDirectory();
  const file = path.join(directory, 'poster.jpg');
  const covers = path.join(directory, 'covers');
  await fs.writeFile(file, jpegBytes);
  const calls = [];

  const cover = await ensurePngCover({
    file,
    coversDirectory: covers,
    convert: async (source, destination) => {
      calls.push({source, destination});
      await fs.writeFile(destination, pngBytes);
    }
  });

  assert.equal(path.dirname(cover), covers);
  assert.match(path.basename(cover), /^cover-[0-9a-f]{16}\.png$/);
  assert.deepEqual(calls, [{source: file, destination: cover}]);
  assert.deepEqual(await fs.readFile(cover), pngBytes);
});

test('the same JPEG content reuses one converted cover', async () => {
  const directory = await temporaryDirectory();
  const covers = path.join(directory, 'covers');
  const first = path.join(directory, 'first.jpg');
  const second = path.join(directory, 'second.jpg');
  await fs.writeFile(first, jpegBytes);
  await fs.writeFile(second, jpegBytes);
  let conversions = 0;
  const convert = async (source, destination) => {
    conversions++;
    await fs.writeFile(destination, pngBytes);
  };

  const a = await ensurePngCover({file: first, coversDirectory: covers, convert});
  const b = await ensurePngCover({file: second, coversDirectory: covers, convert});

  assert.equal(a, b);
  assert.equal(conversions, 1);
});

test('an unsupported image type is rejected before any conversion', async () => {
  const directory = await temporaryDirectory();
  const file = path.join(directory, 'poster.webp');
  await fs.writeFile(file, Buffer.from('RIFF0000WEBP'));

  await assert.rejects(
    ensurePngCover({file, coversDirectory: directory, convert: async () => assert.fail('must not convert')}),
    /仅支持 PNG、JPG、BMP 或 GIF/
  );
});

test('a conversion that fails removes the partial cover and reports the failure', async () => {
  const directory = await temporaryDirectory();
  const file = path.join(directory, 'poster.jpg');
  const covers = path.join(directory, 'covers');
  await fs.writeFile(file, jpegBytes);

  await assert.rejects(ensurePngCover({
    file,
    coversDirectory: covers,
    convert: async (source, destination) => {
      await fs.writeFile(destination, Buffer.from('partial'));
      throw new Error('converter crashed');
    }
  }), /无法把所选图片转换为 PNG/);

  assert.deepEqual(await fs.readdir(covers), []);
});

test('a conversion that produces a non-PNG result is discarded', async () => {
  const directory = await temporaryDirectory();
  const file = path.join(directory, 'poster.jpg');
  const covers = path.join(directory, 'covers');
  await fs.writeFile(file, jpegBytes);

  await assert.rejects(ensurePngCover({
    file,
    coversDirectory: covers,
    convert: async (source, destination) => fs.writeFile(destination, jpegBytes)
  }), /无法把所选图片转换为 PNG/);

  assert.deepEqual(await fs.readdir(covers), []);
});

test('a source larger than the limit is rejected before conversion', async () => {
  const directory = await temporaryDirectory();
  const file = path.join(directory, 'huge.jpg');
  await fs.writeFile(file, jpegBytes);

  await assert.rejects(
    ensurePngCover({file, coversDirectory: directory, convert: async () => assert.fail('must not convert'), maxBytes: 4}),
    /不能大于 1 MB/
  );
});

test('the conversion helper runs System.Drawing with quoted paths', async () => {
  const calls = [];
  const convert = createWindowsPngConverter({
    powershell: 'powershell.exe',
    execFile: async (...args) => { calls.push(args); }
  });

  await convert("C:\\Users\\o'brien\\poster.jpg", 'C:\\covers\\poster.png');

  assert.equal(calls.length, 1);
  const [executable, args, options] = calls[0];
  assert.equal(executable, 'powershell.exe');
  assert.deepEqual(args.slice(0, 3), ['-NoProfile', '-NonInteractive', '-Command']);
  assert.match(args[3], /System\.Drawing\.Imaging\.ImageFormat\]::Png/);
  assert.match(args[3], /C:\\Users\\o''brien\\poster\.jpg/);
  assert.match(args[3], /C:\\covers\\poster\.png/);
  assert.deepEqual(options, {windowsHide: true, timeout: 30000});
});

test('the conversion helper requires a process runner', () => {
  assert.throws(() => createWindowsPngConverter({}), /process runner/);
});
