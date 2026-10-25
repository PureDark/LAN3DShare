// Exact SVG export: node Export-Icon.cjs [absolute path to sharp module]
// No redraw, recoloring, background, padding changes, or AI-generated artwork.
const fs = require('node:fs');
const path = require('node:path');
const sharp = require(process.argv[2] || 'sharp');

(async () => {
  const design = fs.readFileSync(path.resolve(__dirname, '../../..', 'design/lan3dshare-ui.html'), 'utf8');
  const match = design.match(/<div class="app">\s*(<svg\b[\s\S]*?<\/svg>)/);
  if (!match) throw new Error('The original titlebar SVG was not found.');
  const svg = match[1].replace('<svg ', '<svg xmlns="http://www.w3.org/2000/svg" ');
  fs.writeFileSync(path.join(__dirname, 'app.svg'), svg + '\n');
  const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
  const frames = [];
  for (const size of sizes) {
    const png = await sharp(Buffer.from(svg), {density: 1536}).resize(size, size).png().toBuffer();
    const raw = await sharp(png).ensureAlpha().raw().toBuffer();
    let transparent = 0, translucent = 0, opaque = 0;
    for (let i = 3; i < raw.length; i += 4) {
      if (raw[i] === 0) transparent++;
      else if (raw[i] === 255) opaque++;
      else translucent++;
    }
    if (raw[3] !== 0 || raw[raw.length - 1] !== 0 || !transparent || !translucent || !opaque)
      throw new Error(`Invalid alpha channel at ${size}px.`);
    frames.push({size, png});
    if (size === 256) fs.writeFileSync(path.join(__dirname, 'app.png'), png);
    console.log(`${size}x${size}: transparent=${transparent}, translucent=${translucent}, opaque=${opaque}`);
  }
  const header = Buffer.alloc(6 + frames.length * 16);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(frames.length, 4);
  let offset = header.length;
  frames.forEach(({size, png}, i) => {
    const entry = 6 + i * 16;
    header[entry] = size === 256 ? 0 : size;
    header[entry + 1] = size === 256 ? 0 : size;
    header.writeUInt16LE(1, entry + 4);
    header.writeUInt16LE(32, entry + 6);
    header.writeUInt32LE(png.length, entry + 8);
    header.writeUInt32LE(offset, entry + 12);
    offset += png.length;
  });
  fs.writeFileSync(path.join(__dirname, 'app.ico'), Buffer.concat([header, ...frames.map(frame => frame.png)]));
})().catch(error => { console.error(error); process.exitCode = 1; });
