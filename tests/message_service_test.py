# ABOUTME: Exercises text acknowledgments and sequence-based synchronization against a real server.
# ABOUTME: Reuses isolated MySQL, Redis and socket fixtures without touching production records.
import uuid
import unittest
import media_service_test as media


class MessageServiceTest(media.MediaServiceTest):
    def test_text_confirmation(self):
        sender, receiver, outsider, anonymous = self.clients
        target = dict(is_group=False, target=2)
        identifier = str(uuid.uuid4())
        payload = dict(conversation=target, client_msg_id=identifier, text='持久文本测试')
        response = sender.request('send_text', **payload)
        self.assertTrue(response['ok'], response)
        message = response['data']['message']
        self.assertEqual(message['client_msg_id'], identifier)
        self.assertEqual(message['kind'], 'text')
        self.assertGreater(int(message['sequence']), 0)
        repeated = sender.request('send_text', **payload)
        self.assertFalse(repeated['data']['created'])
        self.assertEqual(repeated['data']['message'], message)
        payload['text'] = 'different'
        self.assertEqual(sender.request('send_text', **payload)['error'], 'conflict')
        self.assertEqual(outsider.request('send_text', **payload)['error'], 'forbidden')
        history = receiver.request('sync', conversation=dict(is_group=False, target=1), direction='initial', limit=100)
        self.assertTrue(history['ok'], history)
        self.assertIn(message, history['data']['messages'])

    def test_sync_boundaries(self):
        sender = self.clients[0]
        target = dict(is_group=True, target=7)
        ids = []
        for number in range(5):
            response = sender.request('send_text', conversation=target,
                client_msg_id=str(uuid.uuid4()), text='page ' + str(number))
            self.assertTrue(response['ok'], response)
            ids.append(response['data']['message']['message_id'])
        initial = sender.request('sync', conversation=target, direction='initial', limit=2)['data']
        self.assertEqual([item['message_id'] for item in initial['messages']], ids[-2:])
        before = sender.request('sync', conversation=target, direction='before', cursor=initial['lower'], limit=2)['data']
        self.assertEqual([item['message_id'] for item in before['messages']], ids[-4:-2])
        after = sender.request('sync', conversation=target, direction='after', cursor=before['upper'],
            through=initial['upper'], limit=1)['data']
        self.assertTrue(after['more'])
        self.assertEqual(after['messages'][0]['message_id'], ids[-2])
        end = sender.request('sync', conversation=target, direction='after', cursor=after['upper'],
            through=initial['upper'], limit=1)['data']
        self.assertFalse(end['more'])
        self.assertEqual(end['upper'], initial['upper'])


if __name__ == '__main__':
    unittest.main(argv=['message_service_test.py'] + media.schema.TEST_ARGS)
