# ABOUTME: Runs Windows client storage and rendering checks against an isolated Linux service.
# ABOUTME: Cleans up temporary MySQL accounts and coordinates the offline window disconnection.
import pathlib
import subprocess
import time
import unittest
import media_service_test as media


class ClientStorageTest(unittest.TestCase):
    def test_client_storage(self):
        self.addCleanup(media.MediaServiceTest.doClassCleanups)
        media.MediaServiceTest.setUpClass()
        media.schema.execute('INSERT INTO Friend VALUES(4,5),(5,4);', media.schema.MediaSchemaTest.database)
        output = pathlib.Path('/mnt/f/linux/_environment/logs/client-storage')
        output.mkdir(exist_ok=True)
        command = ['/mnt/c/Users/lds/scoop/apps/nodejs/current/node.exe',
            'E:\\chat_server_qt\\ChatClient\\chat_client_electron\\tests\\run_service.js',
            str(media.MediaServiceTest.control_port), 'F:\\linux\\_environment\\logs\\client-storage']
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=300,
            cwd='/mnt/e/chat_server_qt/ChatClient/chat_client_electron')
        (output / 'online.log').write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr, '', result.stderr)
        ready = output / 'offline-window-ready.json'
        ready.unlink(missing_ok=True)
        with (output / 'offline.log').open('w') as log:
            process = subprocess.Popen(command + ['offline'], stdout=log, stderr=log,
                cwd='/mnt/e/chat_server_qt/ChatClient/chat_client_electron')
            deadline = time.monotonic() + 60
            while not ready.exists() and process.poll() is None and time.monotonic() < deadline:
                time.sleep(.1)
            self.assertTrue(ready.exists(), 'Offline window did not reach authenticated state')
            media.MediaServiceTest.stop()
            self.assertEqual(process.wait(timeout=60), 0, (output / 'offline.log').read_text())
        self.assertEqual((output / 'offline.log').read_text().strip(), '')


if __name__ == '__main__':
    unittest.main(argv=[__file__] + media.schema.TEST_ARGS)
