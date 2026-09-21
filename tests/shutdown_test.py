# ABOUTME: Verifies orderly shutdown while an unauthenticated control connection remains open.
# ABOUTME: Uses the real server and closes test sockets during cleanup even when shutdown fails.
import signal
import unittest
import media_service_test as media


class ShutdownTest(unittest.TestCase):
    def test_anonymous_connection_does_not_hold_shutdown(self):
        self.addCleanup(media.MediaServiceTest.doClassCleanups)
        media.MediaServiceTest.setUpClass()
        for client in media.MediaServiceTest.clients[:3]:
            client.socket.close()
        server = media.MediaServiceTest.server
        server.send_signal(signal.SIGINT)
        self.assertEqual(server.wait(timeout=5), 0)


if __name__ == '__main__':
    unittest.main(argv=[__file__] + media.schema.TEST_ARGS)
