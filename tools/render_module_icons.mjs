// Module icons in the project-logos style: rounded tile with a darker base, a stroked glyph with a soft shadow.
// Writes <out>/<id>/icon.png (128 px), <out>/<id>.svg and a contact sheet at 128/64/32/24 px.
//   node tools/render_module_icons.mjs <out> [legacy.svg]
// Playwright comes from PLAYWRIGHT_DIR (a folder with playwright in its node_modules) or the normal module lookup.
// legacy.svg (optional, also LEGACY_SVG) puts the Legacy Essentials logo first on the contact sheet.
import { createRequire } from 'module';
import fs from 'fs';
import path from 'path';
const require = createRequire(path.join(process.env.PLAYWRIGHT_DIR || process.cwd(), 'package.json'));
const { chromium } = require('playwright');
const OUT = process.argv[2];
if (!OUT) { console.error('usage: node tools/render_module_icons.mjs <out> [legacy.svg]'); process.exit(2); }
const LEGACY_SVG = process.argv[3] || process.env.LEGACY_SVG || '';

const amber = { main: '#f59e0b', base: '#a35f00', ink: '#1f1300' };
const G = {
  save: '<path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7"/><path d="M7 3v4a1 1 0 0 0 1 1h7"/>',
  plug: '<path d="M12 22v-5"/><path d="M9 8V2"/><path d="M15 8V2"/><path d="M18 8v5a4 4 0 0 1-4 4h-4a4 4 0 0 1-4-4V8Z"/>',
  volume: '<path d="M11 4.7a.7.7 0 0 0-1.2-.5L6.4 7.6A1.4 1.4 0 0 1 5.4 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.4a1.4 1.4 0 0 1 1 .4l3.4 3.4a.7.7 0 0 0 1.2-.5z"/><path d="M16 9a5 5 0 0 1 0 6"/><path d="M19.4 18.4a9 9 0 0 0 0-12.8"/>',
  keys: '<rect x="9" y="3.5" width="6" height="6" rx="1.6"/><rect x="2" y="13.5" width="6" height="6" rx="1.6"/><rect x="9" y="13.5" width="6" height="6" rx="1.6"/><rect x="16" y="13.5" width="6" height="6" rx="1.6"/>',
  wheel: '<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.4"/><path d="M12 14.4V21"/><path d="M9.7 11.4 3.4 9.6"/><path d="M14.3 11.4l6.3-1.8"/>',
  timer: '<path d="M10 2h4"/><path d="M12 14l3-3"/><circle cx="12" cy="14" r="8"/>',
  search: '<circle cx="11" cy="11" r="7.5"/><path d="M21 21l-4.6-4.6"/>',
  upload: '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="M17 8l-5-5-5 5"/><path d="M12 3v12"/>',
  gauge: '<path d="M12 14l4-4"/><path d="M3.3 19a10 10 0 1 1 17.4 0"/>',
  code: '<path d="M16 18l6-6-6-6"/><path d="M8 6l-6 6 6 6"/>',
  tree: '<circle cx="12" cy="18" r="3"/><circle cx="6" cy="6" r="3"/><circle cx="18" cy="6" r="3"/><path d="M18 9v2a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V9"/><path d="M12 12v3"/>',
};
const ICONS = [
  { id: 'eswc-autosave', g: 'save', c: amber },
  { id: 'controller-hotplug', g: 'plug', c: amber },
  { id: 'game-settings', g: 'volume', c: amber },
  { id: 'input-display', g: 'wheel', c: { main: '#0ea5e9', base: '#075f8a', ink: '#ffffff' } },
  { id: 'wr-hunter', g: 'timer', c: { main: '#f43f5e', base: '#9f1239', ink: '#ffffff' } },
  { id: 'tmx-browser', g: 'search', c: { main: '#10b981', base: '#05674a', ink: '#ffffff' } },
  { id: 'tmx-upload', g: 'upload', c: { main: '#14b8a6', base: '#0c6b62', ink: '#ffffff' } },
  { id: 'performance', g: 'gauge', c: { main: '#8b5cf6', base: '#4c1d95', ink: '#ffffff' } },
  { id: 'scripts', g: 'code', c: { main: '#6366f1', base: '#312e81', ink: '#ffffff' } },
  { id: 'node-explorer', g: 'tree', c: { main: '#64748b', base: '#334155', ink: '#ffffff' } },
];

function svg(i) {
  const glyph = G[i.g];
  const g = (stroke, extra) =>
    `<g ${extra} fill="none" stroke-linecap="round" stroke-linejoin="round" stroke="${stroke}" stroke-width="2.6">${glyph}</g>`;
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">` +
    `<rect x="18" y="18" width="220" height="220" rx="74" fill="${i.c.base}"/>` +
    `<g transform="translate(0 -8)"><rect x="18" y="18" width="220" height="220" rx="74" fill="${i.c.main}"/></g>` +
    g(i.c.base, 'transform="translate(2.5 5) translate(51.2 43.2) scale(6.4)" opacity=".55"') +
    g(i.c.ink, 'transform="translate(51.2 43.2) scale(6.4)"') + `</svg>`;
}

const b = await chromium.launch();
const pg = await (await b.newContext({ deviceScaleFactor: 1 })).newPage();
async function png(text, size, path) {
  await pg.setViewportSize({ width: size, height: size });
  await pg.setContent(`<html><body style="margin:0;background:transparent">${text.replace('width="256" height="256"', `width="${size}" height="${size}"`)}</body></html>`);
  await pg.screenshot({ path, omitBackground: true, clip: { x: 0, y: 0, width: size, height: size } });
}
fs.mkdirSync(OUT, { recursive: true });
let cells = '';
for (const i of ICONS) {
  const s = svg(i);
  fs.mkdirSync(`${OUT}/${i.id}`, { recursive: true });
  fs.writeFileSync(`${OUT}/${i.id}.svg`, s);
  await png(s, 128, `${OUT}/${i.id}/icon.png`);
  const u = 'data:image/svg+xml;base64,' + Buffer.from(s).toString('base64');
  cells += `<div class="c"><img src="${u}" width="128"><img src="${u}" width="64"><img src="${u}" width="32"><img src="${u}" width="26"><img src="${u}" width="24"><div>${i.id}</div></div>`;
}
if (LEGACY_SVG) {
  const lu = 'data:image/svg+xml;base64,' + fs.readFileSync(LEGACY_SVG).toString('base64');
  cells = `<div class="c"><img src="${lu}" width="128"><img src="${lu}" width="64"><img src="${lu}" width="32"><img src="${lu}" width="26"><img src="${lu}" width="24"><div>Legacy Essentials</div></div>` + cells;
}
await pg.setViewportSize({ width: 1100, height: 600 });
await pg.setContent(`<html><body style="margin:0;background:#141821;font-family:Segoe UI;color:#cfd3dc;font-size:12px"><style>
.w{display:flex;flex-wrap:wrap;gap:18px;padding:20px}.c{display:flex;flex-direction:column;align-items:center;gap:6px;width:140px}</style><div class="w">${cells}</div></body></html>`);
await pg.screenshot({ path: `${OUT}/sheet.png`, fullPage: true });
await b.close();
