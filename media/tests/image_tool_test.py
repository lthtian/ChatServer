# ABOUTME: Runs the image command against real files and validates its JSON contract.
# ABOUTME: Captures expected failures and ensures partial results are not published.
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

TOOL = pathlib.Path(sys.argv.pop(1))


def png(width=640, height=320):
    def chunk(kind, content):
        return (struct.pack('>I', len(content)) + kind + content
                + struct.pack('>I', zlib.crc32(kind + content)))
    rows = (b'\0' + bytes([12, 34, 56, 78]) * width) * height
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class ImageToolTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='chat-image-tool-')
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.source = self.root / '图片.png'
        self.source.write_bytes(png())
        self.output = self.root / 'cache'

    def run_tool(self, source=None, key='thumbnail'):
        return subprocess.run([str(TOOL), str(source or self.source), str(self.output), key],
                              capture_output=True, encoding='utf-8', timeout=15)

    def test_thumbnail(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        info = json.loads(result.stdout)
        self.assertEqual(info['source'], {'width': 640, 'height': 320, 'mime': 'image/png'})
        self.assertEqual(info['thumbnail']['width'], 320)
        self.assertEqual(info['thumbnail']['height'], 160)
        self.assertEqual(info['thumbnail']['mime'], 'image/png')
        self.assertEqual(info['thumbnail']['key'], 'thumbnail')
        body = (self.output / 'objects' / 'thumbnail').read_bytes()
        self.assertEqual(body[:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>II', body[16:24]), (320, 160))
        self.assertEqual(info['thumbnail']['bytes'], len(body))

    def test_corrupt(self):
        self.source.write_bytes(b'\xff\xd8\xff\xe0\x00\x10')
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stderr, '')
        self.assertEqual(json.loads(result.stdout)['error'], 'invalid_image')
        self.assertFalse((self.output / 'objects' / 'thumbnail').exists())

    def test_missing_input(self):
        result = self.run_tool(self.root / 'missing.png')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error'], 'io_error')

    def test_no_overwrite(self):
        self.assertEqual(self.run_tool().returncode, 0)
        path = self.output / 'objects' / 'thumbnail'
        before = path.read_bytes()
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error'], 'io_error')
        self.assertEqual(path.read_bytes(), before)

    def test_rejects_traversal(self):
        result = self.run_tool(key='../escape')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error'], 'invalid_argument')
        self.assertFalse((self.output / 'escape').exists())

    def test_decoder_diagnostics_do_not_break_json(self):
        body = bytearray(png())
        body[45] ^= 0xff
        self.source.write_bytes(body)
        result = self.run_tool()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error'], 'invalid_image')
        self.assertIn('libpng error:', result.stderr)
        self.assertFalse((self.output / 'objects' / 'thumbnail').exists())


if __name__ == '__main__':
    unittest.main()
