// Offline browser bridge tests; does not modify user settings or open a LAN session.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { pathToFileURL } = require('node:url');
const { chromium } = require('playwright');
const root = path.resolve(__dirname, '..');
const output = path.join(root, 'build-opt/test-results/receiver-volume');
fs.mkdirSync(output, { recursive: true });
(async () => {
  const browser = await chromium.launch({ channel: 'msedge', headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: 780, height: 540 } });
    const errors = [], checks = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(pathToFileURL(path.join(root, 'src/ui/web/index.html')).href);
    await page.evaluate(() => {
      window.actions = [];
      window.addEventListener('lan3dshare-action', event => actions.push(event.detail));
      window.render({ mode: 'main', phase: 'receiving', volumePercent: 100, peer: { name: 'PUREDARK-GAMING' }, contentKind: 'sbs' });
    });
    assert.equal(await page.locator('#settings-audio-gain').count(), 0);
    assert.equal(await page.locator('#receive-volume').inputValue(), '100');
    checks.push('receiver control visible, sender setting removed, original volume default');
    const setVolume = async (id, value) => page.evaluate(({ id, value }) => {
      const slider = document.getElementById(id); slider.value = value;
      slider.dispatchEvent(new Event('input', { bubbles: true }));
    }, { id, value });
    for (const volume of [500, 0, 100, 800]) {
      await setVolume('receive-volume', volume);
      assert.deepEqual(await page.evaluate(() => actions.at(-1)), { action: 'volume', percent: volume });
    }
    assert.equal(await page.evaluate(() => actions.some(a => ['disconnect', 'connect', 'saveSettings'].includes(a.action))), false);
    checks.push('live main slider, mute and limits send volume only');
    await page.evaluate(() => window.render({ volumePercent: 375 }));
    assert.equal(await page.locator('#receive-volume').inputValue(), '375');
    checks.push('native volume update reflected by main control');
    await page.screenshot({ path: path.join(output, 'receiving.png') });
    await page.evaluate(() => window.render({ phase: 'sending' }));
    assert.equal(await page.locator('#receive-volume').isDisabled(), true);
    checks.push('sender cannot change receiver playback volume');
    await page.setViewportSize({ width: 480, height: 294 });
    await page.evaluate(() => window.render({ mode: 'viewer', type: 'viewer', peerName: 'A very long receiver peer computer name', kind: 'sbs', fullscreen: false, fallback: false, volumePercent: 350 }));
    await page.mouse.move(230, 25);
    await setVolume('viewer-volume', 600);
    assert.deepEqual(await page.evaluate(() => actions.at(-1)), { action: 'volume', percent: 600 });
    const box = await page.locator('#viewer-volume').boundingBox();
    assert.ok(box && box.x >= 0 && box.x + box.width <= 480);
    await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
    assert.equal(await page.evaluate(() => actions.some(a => a.action === 'drag')), false);
    checks.push('minimum viewer window fits slider; dragging volume does not move window');
    await page.screenshot({ path: path.join(output, 'viewer-minimum.png') });
    await page.evaluate(() => window.render({ fullscreen: true, volumePercent: 500 }));
    assert.equal(await page.locator('.v-bar').isVisible(), false);
    await page.keyboard.press('Control+ArrowUp');
    assert.deepEqual(await page.evaluate(() => actions.at(-1)), { action: 'volume', percent: 525 });
    await page.keyboard.press('Control+ArrowDown');
    assert.deepEqual(await page.evaluate(() => actions.at(-1)), { action: 'volume', percent: 500 });
    checks.push('fullscreen SBS hides overlay but retains keyboard volume');
    assert.deepEqual(errors, []);
    fs.writeFileSync(path.join(output, 'ui.json'), JSON.stringify({ result: 'PASS', checks, scope: 'Offline Edge, mocked native message bridge; native WebView and actual WASAPI covered separately' }, null, 2));
    console.log(`Receiver volume UI: ${checks.length} checks passed`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
