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
const scenes = [
  {state:'idle',file:'readme-connect.png',title:'选择电脑，开始共享',subtitle:'Choose a computer. Share your 3D game.'},
  {state:'receiving',file:'readme-receive.png',title:'收到画面，一键全屏',subtitle:'Receive the picture. Go fullscreen for 3D.'},
  {state:'viewer',file:'readme-viewer.png',title:'自由观看，随时调节',subtitle:'Move the window. Make yourself comfortable.'}
];
(async () => {
  fs.mkdirSync(output,{recursive:true});
  const browser = await chromium.launch({channel:'msedge',headless:true});
  const manifest=[];
  try {
    for(const scene of scenes) {
      const page=await browser.newPage({viewport:{width:900,height:624},deviceScaleFactor:2});
      const errors=[];page.on('pageerror',e=>errors.push(e.message));
      await page.goto(pathToFileURL(path.join(root,'src/ui/web/index.html')).href);
      await page.addStyleTag({content:'*,*::before,*::after{animation:none!important;transition:none!important}'});
      const model=fixture();model.uiState=scene.state;model.selectedSource='game';model.contentKind='sbs';
      if(scene.state==='receiving') {
        model.phase='receiving';model.elapsedSeconds=728;
        model.stats='3840 × 2160 · 60 FPS';
      }
      if(scene.state==='receiving'||scene.state==='viewer') {
        model.audio.receiver=true;model.audio.volume=400;
        model.audio.input={peak:.12,rms:.05,active:true};model.audio.output={peak:.48,rms:.2,active:true};
      }
      if(scene.state==='viewer') {
        await page.setViewportSize({width:900,height:563});
        Object.assign(model,{type:'viewer',mode:'viewer',peerName:'客厅电脑',kind:'sbs',fullscreen:false,fallback:false});
      }
      await page.evaluate(model=>{window.render(model);return document.fonts.ready.then(()=>{});},model);
      if(scene.state==='viewer') await page.mouse.move(300,25);
      assert.deepEqual(errors,[],`${scene.state}: JavaScript errors`);
      const picture=await page.screenshot();
      await page.close();
      // Render a documentation canvas around the real UI; never alter production layout.
      const height=scene.state==='viewer'?821:882;
      const canvas=await browser.newPage({viewport:{width:1044,height},deviceScaleFactor:2});
      await canvas.setContent(`<!doctype html><meta charset="utf-8"><style>
        *{box-sizing:border-box}body{margin:0;padding:48px 72px 70px;background:#EEF1F6;color:#18202B;font-family:"Segoe UI","Microsoft YaHei UI",sans-serif}
        header{height:138px}small{font-size:11px;letter-spacing:2px;color:#65718A;font-weight:600}
        h1{font-size:27px;font-weight:600;letter-spacing:.4px;margin:14px 0 8px}
        p{font-size:14px;color:#7C8697;margin:0}
        .frame{width:900px;border-radius:14px;overflow:hidden;background:#F8F9FB;box-shadow:0 0 0 1px #17233812,0 24px 52px -16px #24385340}
        img{width:900px;height:auto;display:block}
      </style><header><small>LAN3DSHARE / LEIASR 3D SHARING</small><h1>${scene.title}</h1><p>${scene.subtitle}</p></header><div class="frame"><img src="data:image/png;base64,${picture.toString('base64')}" alt="LAN3DShare UI"></div>`);
      await canvas.evaluate(()=>document.fonts.ready);
      await canvas.locator('img').evaluate(img=>img.decode());
      const frame=await canvas.locator('.frame').boundingBox();
      assert.equal(frame.x,72);assert.equal(frame.width,900);
      assert.ok(height-frame.y-frame.height>=70,'Preserve whitespace below the window');
      await canvas.screenshot({path:path.join(output,scene.file)});
      manifest.push({state:scene.state,file:scene.file,width:1044*2,height:height*2,padding:72,sampleData:true});
      await canvas.close();
    }
    fs.writeFileSync(path.join(output,'manifest.json'),JSON.stringify({source:'src/ui/web/index.html',sampleData:true,liveSession:false,screenshots:manifest},null,2)+'\n');
    console.log(`Generated ${manifest.length} framed documentation previews.`);
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
