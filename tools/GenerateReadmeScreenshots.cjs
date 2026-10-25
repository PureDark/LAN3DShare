// Render documentation previews from the shipped HTML UI using sample data.
// NODE_PATH must contain Playwright. Requires Microsoft Edge; no live client,
// game, capture DLL or network session is started.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {pathToFileURL} = require('node:url');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '..');
const output = path.join(root, 'docs/screenshots');
const fixture = () => ({
  type:'state', mode:'main', phase:'idle', designScene:true, localName:'书房电脑',
  networkReady:true, note:'', selectedSource:'screen', selectedPeer:'living',
  sources:[
    {id:'screen',name:'屏幕 1',sub:'3840 × 2160 · 自动识别 3D',kind:'screen',width:3840,height:2160},
    {id:'game',name:'likeadragongaiden.exe',sub:'已加载 LeiaSR，直接采集 3D 画面',kind:'process',width:3840,height:2160},
    {id:'test',name:'3D 测试画面',sub:'无需游戏，检查连接与全屏',kind:'test',width:1280,height:720}
  ],
  peers:[{id:'living',name:'客厅电脑',address:'192.168.1.25',state:'idle'},
    {id:'studio',name:'工作室电脑',address:'192.168.1.31',state:'idle'}],
  peer:{name:'客厅电脑',address:'192.168.1.25'},
  quality:{res:'4k',fps:60,rate:200},
  settings:{name:'书房电脑',monitor:0,runtime:'C:\\Program Files\\LeiaSR\\Platform\\bin',runtimeFound:true,gpu:'auto'},
  monitors:[{id:0,name:'屏幕 1',width:3840,height:2160}],
  gpus:[{id:'pci:0000:01:00.0',name:'GPU 0 · NVIDIA GeForce RTX 5090（PCI 0000:01:00.0）'},
    {id:'pci:0000:03:00.0',name:'GPU 1 · NVIDIA GeForce RTX 5090（PCI 0000:03:00.0）'}],
  contentKind:'desktop', elapsedSeconds:0,stats:'',busy:false,volumePercent:400,
  audio:{receiver:false,enabled:true,ready:true,volume:100,input:{peak:.25,rms:.1,active:true},output:{peak:.25,rms:.1,active:true}}
});
(async () => {
  fs.mkdirSync(output,{recursive:true});
  const browser = await chromium.launch({channel:'msedge',headless:true});
  const manifest=[];
  try {
    for(const state of ['idle','picker','quality','sending','receiving','settings','viewer']) {
      const page=await browser.newPage({viewport:{width:1170,height:810},deviceScaleFactor:1});
      const errors=[];page.on('pageerror',e=>errors.push(e.message));
      await page.goto(pathToFileURL(path.join(root,'src/ui/web/index.html')).href);
      await page.addStyleTag({content:'*,*::before,*::after{animation:none!important;transition:none!important}'});
      const model=fixture();model.uiState=state;
      if(state==='sending'||state==='receiving') {
        model.phase=state;model.contentKind='sbs';model.selectedSource='game';model.elapsedSeconds=728;
        model.stats='NVENC AV1 · 3840 × 2160 · 60 FPS · 200 Mbps';
      }
      if(state==='receiving'||state==='viewer') {
        model.audio.receiver=true;model.audio.volume=400;
        model.audio.input={peak:.12,rms:.05,active:true};model.audio.output={peak:.48,rms:.2,active:true};
      }
      if(state==='viewer') {
        await page.setViewportSize({width:960,height:600});
        Object.assign(model,{type:'viewer',mode:'viewer',peerName:'客厅电脑',kind:'sbs',fullscreen:false,fallback:false});
      }
      await page.evaluate(model=>{window.render(model);return document.fonts.ready.then(()=>{});},model);
      if(state==='viewer') await page.mouse.move(300, 25);
      if(state==='settings') await page.locator('#settings-gpu').selectOption('pci:0000:03:00.0');
      assert.deepEqual(errors,[],`${state}: JavaScript errors`);
      // Document only the quality controls; the shipped panel still has a
      // legacy software-fallback footer that does not describe the GPU path.
      let clip;
      if(state==='quality') {
        const panel=await page.locator('.qpop').boundingBox();
        const note=await page.locator('.q-note').boundingBox();
        clip={x:Math.floor(panel.x),y:Math.floor(panel.y),width:Math.ceil(panel.width),height:Math.floor(note.y-panel.y)};
      }
      await page.screenshot({path:path.join(output,`${state}.png`),...(clip?{clip}:{})});
      manifest.push({state,file:`${state}.png`,width:clip?clip.width:state==='viewer'?960:1170,height:clip?clip.height:state==='viewer'?600:810,croppedControls:!!clip});
      await page.close();
    }
    fs.writeFileSync(path.join(output,'manifest.json'),JSON.stringify({source:'src/ui/web/index.html',sampleData:true,liveSession:false,screenshots:manifest},null,2)+'\n');
    console.log(`Generated ${manifest.length} documentation previews; no live client or network session.`);
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
