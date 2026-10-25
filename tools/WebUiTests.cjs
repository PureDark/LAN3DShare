// Offline browser verification of the embedded UI against the supplied design.
// NODE_PATH must include Playwright, pngjs and pixelmatch; no live LAN client is opened.
const fs = require('fs');
const path = require('path');
const assert = require('assert/strict');
const { pathToFileURL } = require('url');
const { chromium } = require('playwright');
const { PNG } = require('pngjs');
const root = path.resolve(__dirname, '..');
const output = path.join(root, 'build/test-results/web-ui');
fs.mkdirSync(output, { recursive: true });
const defaultFixture = () => ({
  type: 'state', phase: 'idle', designScene: true, localName: '书房电脑', networkReady: true, note: '', selectedSource: 's1', selectedPeer: 'p1',
  sources: [
    { id: 's1', name: '屏幕 1', sub: '2560 × 1440 · 自动识别 3D', kind: 'screen', width: 2560, height: 1440 },
    { id: 's2', name: '屏幕 2', sub: '3840 × 2160 · 自动识别 3D', kind: 'screen', width: 3840, height: 2160 },
    { id: 'game', name: 'Cyberpunk2077.exe', sub: '已加载 LeiaSR，直接采集 3D 画面', kind: 'process', width: 3840, height: 2160 },
    { id: 'test', name: '3D 测试画面', sub: '无需游戏，检查连接与全屏', kind: 'test', width: 1280, height: 720 }
  ],
  peers: [
    { id: 'p1', name: '客厅电脑', address: '192.168.1.25', state: 'idle' },
    { id: 'p2', name: '工作室 · Odyssey 3D', address: '192.168.1.31', state: 'idle' },
    { id: 'p3', name: '小明的笔记本', address: '192.168.1.40', state: 'receiving', icon: 'laptop' }
  ],
  quality: { res: '4k', fps: 60, rate: 200 },
  settings: { name: '书房电脑', monitor: 2, runtime: 'C:\\Program Files\\LeiaSR\\Platform\\bin', runtimeFound: true },
  monitors: [{ id: 1, name: '屏幕 1', width: 2560, height: 1440 }, { id: 2, name: '屏幕 2', width: 3840, height: 2160 }],
  peer: { name: '客厅电脑', address: '192.168.1.25' }, contentKind: 'desktop', elapsedSeconds: 0, stats: '', busy: false
});
(async () => {
  const pixelmatchModule = await import(pathToFileURL(require.resolve('pixelmatch')).href);
  const pixelmatch = pixelmatchModule.default;
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  const context = await browser.newContext({ viewport: { width: 828, height: 700 }, deviceScaleFactor: 1 });
  const reference = await context.newPage();
  const app = await context.newPage();
  const errors = [];
  app.on('pageerror', error => errors.push(error.message));
  await reference.goto(pathToFileURL(path.join(root, 'design/lan3dshare-ui.html')).href);
  await app.goto(pathToFileURL(path.join(root, 'src/ui/web/index.html')).href);
  const noAnimations = '*,*::before,*::after{animation:none!important;transition:none!important}';
  await reference.addStyleTag({ content: noAnimations + '.win,.viewer{position:absolute;top:0;left:0;box-shadow:none}' });
  await app.addStyleTag({ content: noAnimations + 'body{background:var(--page)}' });
  const results = [];
  for (const state of ['idle', 'picker', 'quality', 'searching', 'sending', 'receiving', 'settings', 'viewer', 'viewer-fallback']) {
    const isViewer = state.startsWith('viewer');
    const fixture = defaultFixture();
    fixture.uiState = state;
    if (state === 'sending') {
      fixture.phase = 'sending'; fixture.contentKind = 'sbs'; fixture.stats = 'NVENC HEVC · 2560 × 1440 · 60 FPS · 200 Mbps';
    } else if (state === 'receiving') {
      fixture.phase = 'receiving'; fixture.contentKind = 'sbs'; fixture.elapsedSeconds = 728; fixture.stats = 'NVENC HEVC · 3840 × 2160 · 60 FPS · 200 Mbps';
    } else if (state === 'searching') { fixture.peers = []; fixture.selectedPeer = ''; }
    if (isViewer) Object.assign(fixture, { type: 'viewer', mode: 'viewer', peerName: '客厅电脑', kind: 'sbs', fullscreen: false, fallback: state === 'viewer-fallback' });
    await app.setViewportSize({ width: isViewer ? 720 : 780, height: isViewer ? 440 : 540 });
    await reference.evaluate(state => { setState('idle'); setState(state); clearInterval(ticker); }, state);
    await app.evaluate(model => window.render(model), fixture);
    const referencePath = path.join(output, state + '-reference.png');
    const actualPath = path.join(output, state + '-actual.png');
    await reference.locator(isViewer ? '.viewer' : '.win').screenshot({ path: referencePath });
    await app.screenshot({ path: actualPath });
    const a = PNG.sync.read(fs.readFileSync(referencePath));
    const b = PNG.sync.read(fs.readFileSync(actualPath));
    assert.equal(a.width, b.width); assert.equal(a.height, b.height);
    // The user explicitly replaced only the bitrate segment with a 0–1000 slider.
    // Exclude that control and its current-value label; retain all surrounding pixels.
    if (state === 'quality') {
      const boxes = [await app.locator('.rate-control').boundingBox(), await app.locator('#rate-value').boundingBox()];
      for (const box of boxes) for (let y = Math.floor(box.y); y < Math.ceil(box.y + box.height); y++) for (let x = Math.floor(box.x); x < Math.ceil(box.x + box.width); x++) {
        const i = (y * a.width + x) * 4; b.data.copy(a.data, i, i, i + 4);
      }
    }
    // Native DWM draws the exterior shadow; compare inside the rounded window only.
    for (let y = 0; y < a.height; y++) for (let x = 0; x < a.width; x++) {
      if (x < 12 || y < 12 || x >= a.width - 12 || y >= a.height - 12) {
        const i = (y * a.width + x) * 4; b.data.copy(a.data, i, i, i + 4);
      }
    }
    const diff = new PNG({ width: a.width, height: a.height });
    const pixels = pixelmatch(a.data, b.data, diff.data, a.width, a.height, { threshold: 0.1, includeAA: true });
    fs.writeFileSync(path.join(output, state + '-diff.png'), PNG.sync.write(diff));
    results.push({ state, differingPixels: pixels, fraction: pixels / (a.width * a.height) });
    assert.equal(pixels, 0, `${state}: visual differences outside the explicitly changed bitrate control`);
  }
  fs.writeFileSync(path.join(output, 'comparisons.json'), JSON.stringify(results, null, 2));
  fs.writeFileSync(path.join(output, 'design-fixture.json'), JSON.stringify(defaultFixture(), null, 2));
  // Exercise the actual native bridge shape, including drafts through periodic refresh.
  await app.setViewportSize({ width: 780, height: 540 });
  const live = defaultFixture(); live.designScene = false; delete live.uiState;
  await app.evaluate(model => { window.actions = []; window.addEventListener('lan3dshare-action', e => window.actions.push(e.detail)); window.render({ ...model, mode: 'main' }); }, live);
  await app.locator('#open-settings').click();
  await app.locator('#settings-name').fill('编辑中的名称');
  await app.locator('#settings-runtime').fill('F:\\DraftRuntime');
  await app.evaluate(model => window.render(model), live);
  assert.equal(await app.locator('#settings-name').inputValue(), '编辑中的名称');
  assert.equal(await app.locator('#settings-runtime').inputValue(), 'F:\\DraftRuntime');
  await app.evaluate(() => window.render({ type: 'runtimeSelected', path: 'F:\\ChosenRuntime', found: true }));
  assert.equal(await app.locator('#settings-name').inputValue(), '编辑中的名称');
  assert.equal(await app.locator('#settings-runtime').inputValue(), 'F:\\ChosenRuntime');
  await app.locator('#save-settings').click();
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'saveSettings', name: '编辑中的名称', monitor: 2, runtime: 'F:\\ChosenRuntime' });
  assert.equal(await app.locator('#modal-wrap').isVisible(), true, 'Saving must wait for native acknowledgement.');
  await app.evaluate(model => window.render(model), live);
  assert.equal(await app.locator('#settings-name').inputValue(), '编辑中的名称');
  await app.evaluate(() => window.render({ type: 'settingsSaved', ok: false, error: '设置文件不可写。' }));
  assert.equal(await app.locator('#modal-wrap').isVisible(), true);
  assert.equal(await app.locator('#settings-name').inputValue(), '编辑中的名称');
  assert.equal(await app.locator('#settings-runtime').inputValue(), 'F:\\ChosenRuntime');
  assert.equal(await app.locator('#settings-error').textContent(), '设置文件不可写。');
  await app.locator('#settings-name').fill('中'.repeat(33));
  assert.equal(await app.locator('#save-settings').isDisabled(), true);
  assert.match(await app.locator('#settings-error').textContent(), /96/);
  await app.locator('#settings-name').fill('中'.repeat(32));
  assert.equal(await app.locator('#save-settings').isDisabled(), false);
  await app.locator('#settings-name').fill('   ');
  assert.equal(await app.locator('#save-settings').isDisabled(), true);
  await app.locator('#settings-name').fill('Bad\x7fName');
  assert.equal(await app.locator('#save-settings').isDisabled(), true);
  await app.locator('#settings-name').fill('编辑中的名称');
  assert.equal(await app.locator('#settings-error').isVisible(), false);
  await app.locator('#settings-runtime').fill('x'.repeat(20000));
  const beforeOversizedSave = await app.evaluate(() => window.actions.length);
  await app.locator('#save-settings').click();
  assert.equal(await app.evaluate(() => window.actions.length), beforeOversizedSave);
  assert.equal(await app.locator('#save-settings').isEnabled(), true);
  assert.match(await app.locator('#settings-error').textContent(), /路径过长/);
  await app.locator('#settings-runtime').fill('F:\\ChosenRuntime');
  await app.locator('#save-settings').click();
  await app.evaluate(() => window.render({ type: 'settingsSaved', ok: true }));
  assert.equal(await app.locator('#modal-wrap').isVisible(), false);
  await app.evaluate(() => window.render({ type: 'openSettings' }));
  await app.locator('#save-settings').click();
  await app.evaluate(() => window.render({ phase: 'preparingReceive', busy: true }));
  await app.evaluate(() => window.render({ type: 'settingsSaved', ok: false, error: '正在共享，请稍后重试。' }));
  await app.evaluate(model => window.render(model), live);
  await app.evaluate(() => window.render({ type: 'openSettings' }));
  assert.equal(await app.locator('#modal-wrap').isVisible(), true);
  assert.equal(await app.locator('#settings-name').isEnabled(), true);
  assert.equal(await app.locator('#save-settings').isEnabled(), true);
  await app.locator('.btn.plain.js-close').click();
  await app.locator('#q-chip').click();
  await app.locator('#rate-slider').evaluate(el => { el.value = '400'; el.dispatchEvent(new Event('input', { bubbles: true })); });
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'quality', rate: 400 });
  await app.locator('#rate-slider').focus();
  await app.keyboard.press('ArrowRight');
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'quality', rate: 401 });
  await app.locator('#rate-slider').dispatchEvent('pointerdown');
  await app.locator('#rate-slider').evaluate(el => { el.value = '393'; el.dispatchEvent(new Event('input', { bubbles: true })); });
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'quality', rate: 400 });
  await app.locator('#rate-slider').dispatchEvent('pointerup');
  await app.locator('#rate-slider').evaluate(el => { el.value = '10'; el.dispatchEvent(new Event('input', { bubbles: true })); });
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'quality', rate: 20 });
  await app.keyboard.press('ArrowLeft');
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'quality', rate: 0 });
  assert.equal(await app.locator('#rate-value').textContent(), '自动');
  await app.keyboard.press('ArrowRight');
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'quality', rate: 20 });
  assert.equal(await app.locator('.rate-ticks i').count(), 10);
  await app.locator('#scrim').click({ position: { x: 10, y: 10 } });
  await app.locator('[data-peer="p2"]').click();
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'selectPeer', id: 'p2' });
  await app.locator('#connect').click();
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'connect' });
  assert.equal(await app.locator('body').getAttribute('data-state'), 'idle');
  assert.equal(await app.locator('#preview .eye').count(), 0, 'A normal screen must not be mislabeled SBS after clicking connect.');
  await app.evaluate(() => window.render({ type: 'viewer', mode: 'viewer', peerName: '接收测试', kind: 'sbs', fullscreen: true, fallback: false, designScene: false }));
  assert.equal(await app.locator('.v-bar').isVisible(), false);
  await app.keyboard.press('Escape');
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'escape' });
  await app.evaluate(() => window.render({ fullscreen: false, kind: 'desktop' }));
  assert.equal(await app.locator('#viewer-kind').textContent(), '2D');
  assert.equal(await app.locator('#viewer-window > svg').isVisible(), false);
  await app.keyboard.press('F11');
  assert.deepEqual(await app.evaluate(() => window.actions.at(-1)), { action: 'fullscreen' });
  assert.deepEqual(errors, []);
  const result = { reference: 'design/lan3dshare-ui.html', browser: await browser.version(), geometry: '780x540 main; 720x440 viewer; deviceScaleFactor 1', comparisonThreshold: 0.1, excludedRegions: '12px native window exterior; user-requested bitrate slider and value label only in quality state', comparisons: results, interactions: 'PASS: native messages, settings drafts and save acknowledgement/failure/UTF8 validation, honest connection states, transparent viewer, bitrate snapping/keyboard/auto/minimum', pageErrors: errors };
  fs.writeFileSync(path.join(output, 'results.json'), JSON.stringify(result, null, 2));
  console.log(JSON.stringify(result, null, 2));
  await browser.close();
})().catch(error => { console.error(error); process.exit(1); });
