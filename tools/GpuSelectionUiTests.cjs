const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {pathToFileURL} = require('node:url');
const {chromium} = require('playwright');
(async () => {
 const browser = await chromium.launch({channel:'msedge',headless:true});
 try {
  const page = await browser.newPage({viewport:{width:1170,height:810}});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto(pathToFileURL(path.resolve('src/ui/web/index.html')).href);
  await page.evaluate(()=>{
   window.actions=[];window.addEventListener('lan3dshare-action',e=>actions.push(e.detail));
   window.render({mode:'main',phase:'idle',localName:'GPU 测试',settings:{name:'GPU 测试',monitor:0,runtime:'C:\\LeiaSR',runtimeFound:true,gpu:'auto'},monitors:[{id:0,name:'屏幕 1',width:3840,height:2160}],gpus:[{id:'pci:0000:01:00.0',name:'GPU 0 · NVIDIA GeForce RTX 5090（PCI 0000:01:00.0）'},{id:'pci:0000:03:00.0',name:'GPU 1 · NVIDIA GeForce RTX 5090（PCI 0000:03:00.0）'}]});
  });
  await page.locator('#open-settings').click();
  assert.equal(await page.locator('#settings-gpu option').count(),3);
  assert.match(await page.locator('#settings-gpu option').nth(1).textContent(), /RTX 5090（PCI 0000:01:00.0）/);
  assert.match(await page.locator('#settings-gpu option').nth(2).textContent(), /RTX 5090（PCI 0000:03:00.0）/);
  assert.equal(await page.locator('#settings-gpu').inputValue(),'auto');
  await page.locator('#settings-gpu').selectOption('pci:0000:03:00.0');
  await page.locator('#save-settings').click();
  assert.equal((await page.evaluate(()=>actions.at(-1))).gpu,'pci:0000:03:00.0');
  assert.equal(await page.locator('#settings-gpu').isDisabled(),true);
  await page.evaluate(()=>window.render({type:'settingsSaved',ok:false,error:'测试保存失败'}));
  assert.equal(await page.locator('#settings-gpu').inputValue(),'pci:0000:03:00.0');
  assert.equal(await page.locator('#settings-gpu').isDisabled(),false);
  await page.setViewportSize({width:780,height:540});
  await page.locator('#settings-gpu').scrollIntoViewIfNeeded();
  assert.equal(await page.locator('#settings-gpu').isVisible(),true);
  assert.deepEqual(errors,[]);
  const out=path.resolve('build-opt/test-results/gpu-selection');fs.mkdirSync(out,{recursive:true});
  await page.screenshot({path:path.join(out,'settings.png')});
  fs.writeFileSync(path.join(out,'result.json'),JSON.stringify({passed:6,checks:['physical adapters','automatic default','selected GPU saved','save locks field','failure preserves choice','small window scroll']},null,2));
  console.log('GPU selection UI: 6 checks passed');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
