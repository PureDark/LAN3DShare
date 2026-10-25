const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {pathToFileURL} = require('node:url');
const {chromium} = require('playwright');
(async () => {
  const root = path.resolve(__dirname, '..');
  const out = path.join(root, 'build-opt/test-results/audio-continuity');
  fs.mkdirSync(out, {recursive:true});
  const browser = await chromium.launch({channel:'msedge',headless:true});
  try {
    const page = await browser.newPage({viewport:{width:1170,height:810}});
    const errors=[]; page.on('pageerror', e=>errors.push(e.message));
    await page.addInitScript(()=>{window.sent=[];window.chrome={webview:{postMessage:m=>window.sent.push(m),addEventListener:()=>{}}};});
    await page.goto(pathToFileURL(path.join(root,'src/ui/web/index.html')).href);
    const state={type:'state',phase:'idle',networkReady:true,localName:'书房电脑',busy:false,
      quality:{res:'4k',fps:60,rate:200,codec:'auto'},sources:[],peers:[],settings:{},monitors:[]};
    await page.evaluate(m=>window.render(m),state);
    await page.locator('#q-chip').click();
    for (const codec of ['av1','h264','hevc','auto']) {
      await page.locator(`[data-key="codec"] [data-v="${codec}"]`).click();
      assert.deepEqual(await page.evaluate(()=>window.sent.at(-1)), {action:'quality',codec});
      assert.equal(await page.locator(`[data-key="codec"] [data-v="${codec}"]`).getAttribute('class'),'on');
    }
    state.quality.codec='hevc'; await page.evaluate(m=>window.render(m),state);
    assert.match(await page.locator('#q-summary').innerText(),/HEVC/);
    const box=await page.locator('.qpop').boundingBox();
    assert.ok(box.y>=0 && box.y+box.height<=810,'quality panel clipped');
    await page.screenshot({path:path.join(out,'codec-settings.png')});
    await page.locator('#q-reset').click();
    assert.equal((await page.evaluate(()=>window.sent.at(-1))).codec,'auto');
    state.phase='receiving'; await page.evaluate(m=>window.render(m),state);
    assert.equal(await page.locator('#q-chip').isDisabled(),true);
    assert.deepEqual(errors,[]);
    fs.writeFileSync(path.join(out,'ui.json'),JSON.stringify({passed:true,codecs:4,restored:true,reset:true,busyGuard:true,panel:box},null,2));
    console.log('Codec UI passed: format messages, restored selection, defaults, session guard and layout');
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
