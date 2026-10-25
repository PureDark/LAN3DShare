"""Read-only PE inventory. Does not load DLLs, inspect account data, or attach processes."""
import hashlib
import json
import re
import struct
from pathlib import Path


class PE:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.pe = self.u32(60)
        if self.data[:2] != b'MZ' or self.data[self.pe:self.pe + 4] != b'PE\0\0':
            raise ValueError('Not a PE file')
        self.machine, count = struct.unpack_from('<HH', self.data, self.pe + 4)
        optional_size = struct.unpack_from('<H', self.data, self.pe + 20)[0]
        optional = self.pe + 24
        self.is64 = struct.unpack_from('<H', self.data, optional)[0] == 523
        self.directory = optional + (112 if self.is64 else 96)
        self.sections = []
        for i in range(count):
            off = optional + optional_size + i * 40
            name, virtual_size, rva, size, raw = struct.unpack_from('<8sIIII', self.data, off)
            self.sections.append((rva, max(virtual_size, size), raw, name.rstrip(b'\0').decode('ascii', 'replace')))

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def offset(self, rva):
        for start, size, raw, _ in self.sections:
            if start <= rva < start + size:
                return raw + rva - start
        if rva < len(self.data):
            return rva
        raise ValueError('RVA outside image')

    def cstr(self, offset):
        end = self.data.find(b'\0', offset)
        return self.data[offset:end].decode('ascii', 'replace')

    def imports(self):
        rva = self.u32(self.directory + 8)
        if not rva:
            return {}
        off = self.offset(rva)
        result = {}
        width = 8 if self.is64 else 4
        for _ in range(4096):
            original, _, _, name, first = struct.unpack_from('<IIIII', self.data, off)
            if not name:
                break
            dll = self.cstr(self.offset(name))
            thunk = self.offset(original or first)
            names = []
            for i in range(65536):
                value = struct.unpack_from('<Q' if self.is64 else '<I', self.data, thunk + i * width)[0]
                if not value:
                    break
                if value & (1 << (width * 8 - 1)):
                    names.append({'ordinal': value & 65535})
                else:
                    names.append(self.cstr(self.offset(value) + 2))
            result[dll] = names
            off += 20
        return result

    def exports(self):
        rva = self.u32(self.directory)
        if not rva:
            return []
        off = self.offset(rva)
        base, count, names_count, funcs, names, ordinals = struct.unpack_from('<IIIIII', self.data, off + 16)
        result = []
        for i in range(names_count):
            name = self.cstr(self.offset(self.u32(self.offset(names) + i * 4)))
            index = struct.unpack_from('<H', self.data, self.offset(ordinals) + i * 2)[0]
            result.append({'name': name, 'ordinal': base + index, 'rva_decimal': self.u32(self.offset(funcs) + index * 4)})
        return result

    def signals(self):
        groups = {
            'capture': r'DuplicateOutput|AcquireNextFrame|DesktopCaptur|ScreenCaptur|WindowsGraphicsCapture|GraphicsCaptureItem|D3D11Captur|D3D12Captur|DXGI.*Captur',
            'codec': r'NvEncodeAPI|nvEncodeAPI|NvEnc|AMFInit|amfrt64|mfxInit|MFXInit|qsv|libx264|libx265|avcodec_(send|receive|open)|MediaFoundation|D3D11VideoDecoder',
            'transport': r'PeerConnection|CreatePeerConnection|libwebrtc|RUDP|DTLS|Srtp|SRTP|IceTransport|P2PTransport|rtc::|webrtc::',
            'video_interfaces': r'ExternalVideo|PushVideo|pushVideo|VideoFrameObserver|VideoSink|OnVideoFrame|onVideoFrame|VideoSource|CustomVideo|SetVideoFrame|setVideoFrame',
            'leiasr': r'create.*Weaver|delete.*Weaver|weave|setInputViewTexture|setOutputFrameBuffer|CreateSharedHandle|OpenSharedResource',
        }
        result = {key: [] for key in groups}
        patterns = {key: re.compile(pattern, re.I) for key, pattern in groups.items()}
        seen = {key: set() for key in groups}
        for match in re.finditer(rb'[\x20-\x7e]{7,}', self.data):
            if len(match.group()) > 600:
                continue
            text = match.group().decode('ascii')
            for key, pattern in patterns.items():
                if pattern.search(text) and text not in seen[key]:
                    seen[key].add(text)
                    if len(result[key]) < 70:
                        result[key].append({'file_offset_decimal': match.start(), 'text': text})
        return {'selected_strings': result, 'unique_match_counts': {key: len(value) for key, value in seen.items()}}

    def inventory(self):
        return {'path': str(self.path), 'size': len(self.data), 'sha256': hashlib.sha256(self.data).hexdigest(),
                'machine_decimal': self.machine, 'is_64_bit': self.is64,
                'imports': self.imports(), 'exports': self.exports(), **self.signals()}


if __name__ == '__main__':
    targets = [
        r'C:\Program Files\ToDesk\ToDesk.exe',
        r'C:\Program Files\ToDesk\zrtc.dll',
        r'C:\Program Files\ToDesk\codec_checker.exe',
        r'C:\Program Files\LeiaSR\Platform\bin\SimulatedRealityDirectX.dll',
        r'C:\Program Files\LeiaSR\Platform\bin\SimulatedRealityOpenGL.dll',
    ]
    results = [PE(path).inventory() for path in targets]
    output = Path(__file__).with_name('local-binary-evidence.json')
    output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    for result in results:
        print(json.dumps({'path': result['path'], 'imports': list(result['imports']), 'export_count': len(result['exports']),
                          'unique_match_counts': result['unique_match_counts']}, ensure_ascii=False))
