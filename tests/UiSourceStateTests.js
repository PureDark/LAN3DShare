// CPU-only source checks. Does not build or launch the application, a browser, or a game.
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const vm = require('node:vm');
const assert = require('node:assert/strict');

const root = path.resolve(__dirname, '..');
const appPath = path.join(root, 'src/ui/App.cpp');
const htmlPath = path.join(root, 'src/ui/web/index.html');
const app = fs.readFileSync(appPath, 'utf8');
const html = fs.readFileSync(htmlPath, 'utf8');
const cases = [];
function check(name, fn) { fn(); cases.push({ name, result: 'PASS' }); }
const regexSource = app.match(/capturePattern\(LR"\(([\s\S]*?)\)"\)/)[1];
const states = input => [...input.matchAll(new RegExp(regexSource, 'g'))].map(m => m[1]);

check('waiting_input_then_gpu_sbs', () => assert.deepEqual(states('SOURCE_STATE waiting-input pid=5332\r\nSOURCE_STATE leia-sbs gpu_shared=1\r\n'), ['waiting-input', 'leia-sbs']));
check('tabs_multiple_spaces_and_no_trailing_space', () => assert.deepEqual(states('SOURCE_STATE waiting-input\r\nSOURCE_STATE\t  leia-sbs\n'), ['waiting-input', 'leia-sbs']));
check('end_of_log_state_without_parameters', () => assert.deepEqual(states('SOURCE_STATE leia-sbs'), ['leia-sbs']));
check('reject_unanchored_and_invalid_state_suffix', () => assert.deepEqual(states('NOT_SOURCE_STATE leia-sbs\nSOURCE_STATE leia-sbs_bad\n'), []));
check('waiting_process_then_gpu_sbs', () => assert.deepEqual(states('SOURCE_STATE waiting-process pid=0\r\nSOURCE_STATE leia-sbs gpu_shared=1\r\n'), ['waiting-process', 'leia-sbs']));
check('waiting_runtime_line_variants', () => assert.deepEqual(states('SOURCE_STATE waiting-runtime pid=54896\r\nSOURCE_STATE\twaiting-runtime'), ['waiting-runtime', 'waiting-runtime']));
check('process_runtime_input_then_gpu_sbs', () => assert.deepEqual(states('SOURCE_STATE waiting-process\nSOURCE_STATE waiting-runtime pid=54896\nSOURCE_STATE waiting-input\nSOURCE_STATE leia-sbs gpu_shared=1\n'), ['waiting-process', 'waiting-runtime', 'waiting-input', 'leia-sbs']));

const noticeStart = app.indexOf('sourceNotice = state ==');
const noticeBlock = app.slice(noticeStart, app.indexOf('\n            }', noticeStart));
const notices = Object.fromEntries([...noticeBlock.matchAll(/state == L"([a-z-]+)" \? L"([^"]*)"/g)].map(m => [m[1], m[2]]));
check('waiting_process_message_is_present_in_source', () => assert.equal(notices['waiting-process'], '源程序正在切换进程，等待接入实际游戏…'));
check('waiting_runtime_message_is_present_in_source', () => assert.equal(notices['waiting-runtime'], '正在等待源程序完成初始化并启用 LeiaSR…'));
check('gpu_sbs_clears_waiting_notice_via_existing_default', () => { assert.equal(notices['leia-sbs'], undefined); assert.match(noticeBlock, /: L"";/); });

const previewStart = html.indexOf('function renderPreview()');
const code = html.slice(previewStart, html.indexOf('\nfunction renderQuality()', previewStart));
const nodes = new Map();
const context = vm.createContext({
    model: {}, previewSignature: '', receivedPreviewSignature: '', ic3d: '3D-icon ',
    validImage: x => typeof x === 'string' && x.startsWith('data:image/'),
    previewImage: () => '', pattern: () => '', scene: () => '', text: () => {},
    $: id => { if (!nodes.has(id)) nodes.set(id, { innerHTML: '' }); return nodes.get(id); }
});
vm.runInContext(code, context);
function badge(phase, kind, image, contentKind = 'desktop') {
    context.model = { phase, contentKind, preview: image, receivedPreview: '', selectedSource: 'selected', sources: [{ id: 'selected', kind }] };
    vm.runInContext('renderPreview()', context);
    return nodes.get('#preview').innerHTML;
}
check('idle_program_indicates_launch_on_connect', () => assert.match(badge('idle', 'program', ''), /连接后启动/));
check('connected_program_without_frame_indicates_waiting', () => { const b = badge('sending', 'program', ''); assert.match(b, /等待画面/); assert.doesNotMatch(b, /连接后启动/); });
check('connected_program_with_desktop_indicates_regular_image', () => assert.match(badge('sending', 'program', 'data:image/jpeg;base64,a'), /普通画面/));
check('connected_program_with_sbs_indicates_stereo', () => assert.match(badge('sending', 'program', 'data:image/jpeg;base64,a', 'sbs'), /3D 左右并排/));
check('connected_process_with_sbs_indicates_stereo', () => assert.match(badge('sending', 'process', 'data:image/jpeg;base64,a', 'sbs'), /3D 左右并排/));
check('program_picker_does_not_promise_first_frame_capture', () => assert.doesNotMatch(html, /连接时启动，从第一帧开始采集/));

const hash = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex').toUpperCase();
const output = process.argv[2] ? path.resolve(process.argv[2]) : path.join(root, 'build-opt/test-results/ui-source-state/result.json');
fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, JSON.stringify({
    result: 'PASS', generatedAt: new Date().toISOString(),
    scope: 'CPU-only source-level regression: actual App.cpp regex evaluated with JavaScript ECMAScript regex, static notice mapping verification, actual HTML renderPreview() executed in isolated VM with DOM/media stubs.',
    limitations: ['Not a native C++ std::regex execution or binary build.', 'No browser/WebView2 rendering, game operation, capture, GPU, connection or remote display test.'],
    node: process.version,
    script: { path: __filename, sha256: hash(__filename) },
    sourceFiles: [appPath, htmlPath].map(p => ({ path: p, sha256: hash(p) })),
    sourceStatePattern: regexSource, caseCount: cases.length, cases
}, null, 2) + '\n', 'utf8');
console.log(`PASS ${cases.length} CPU source/UI checks; evidence: ${output}`);
