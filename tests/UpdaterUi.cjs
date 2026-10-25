const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {pathToFileURL} = require('node:url');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '..');
const output = path.join(root, 'build-opt/test-results/updater');
(async () => {
  fs.mkdirSync(output,{recursive:true});
  const browser = await chromium.launch({channel:'msedge',headless:true});
  try {
    const page=await browser.newPage({viewport:{width:1170,height:810},deviceScaleFactor:1});
    const errors=[];page.on('pageerror',e=>errors.push(e.message));
    await page.addInitScript(()=>{window.sent=[];window.chrome={webview:{postMessage:m=>window.sent.push(m),addEventListener:()=>{}}};});
    await page.goto(pathToFileURL(path.join(root,'src/ui/web/index.html')).href);
    const model={type:'state',mode:'main',phase:'idle',networkReady:true,localName:'书房电脑',busy:false,
      uiState:'settings',settings:{name:'书房电脑',monitor:0,gpu:'auto',runtime:'',runtimeFound:false},
      quality:{res:'4k',fps:60,rate:200},sources:[],peers:[],monitors:[{id:0,name:'屏幕 1',width:3840,height:2160}],gpus:[],
      update:{current:'0.8.0',status:'idle',message:'点击检查 GitHub 最新正式版本',busy:false}};
    const render=()=>page.evaluate(m=>window.render(m),model);
    await render();
    await page.evaluate(()=>window.render({type:'openSettings'}));
    await page.locator('#update-check').click();
    assert.equal(await page.evaluate(()=>window.sent.at(-1).action),'updateCheck');
    model.update={...model.update,status:'available',latest:'0.8.1',hasUpdate:true,message:'发现新版本，可下载后安装'};
    await render();await page.locator('#update-download').click();
    assert.equal(await page.evaluate(()=>window.sent.at(-1).action),'updateDownload');
    model.update={...model.update,status:'downloading',busy:true,progress:43,message:'正在下载更新…'};
    await render();
    assert.equal(await page.locator('#update-progress').isVisible(),true);
    assert.equal(await page.locator('#update-download').isDisabled(),true);
    assert.equal(await page.locator('#update-progress').getAttribute('value'),'43');
    model.update={...model.update,status:'ready',busy:false,progress:100,message:'下载并校验完成，可以安装并重启'};
    await render();await page.locator('#update-install').click();
    assert.equal(await page.evaluate(()=>window.sent.at(-1).action),'updateInstall');
    assert.equal(await page.locator('#update-auth, #update-token').count(),0);
    model.update={...model.update,status:'error',message:'访问 GitHub 失败（HTTP 404），请稍后重试'};
    await render();assert.equal(await page.locator('#update-check').isEnabled(),true);
    model.phase='receiving';await render();
    assert.equal(await page.locator('#update-install').isDisabled(),true);
    model.phase='idle';model.update={...model.update,status:'idle',hasUpdate:false,message:'点击检查 GitHub 最新正式版本'};await render();
    await page.evaluate(()=>window.render({type:'openSettings'}));
    await page.locator('#update-panel').scrollIntoViewIfNeeded();
    await page.screenshot({path:path.join(output,'settings-update.png')});
    assert.deepEqual(errors,[]);
    fs.writeFileSync(path.join(output,'ui.json'),JSON.stringify({passed:true,cases:['check','download','progress','ready-install','no-authorization-ui','retry','receiving-blocks-update'],liveSession:false},null,2));
    console.log('Updater UI passed: controls, progress, no authorization UI, session gate');
  } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
