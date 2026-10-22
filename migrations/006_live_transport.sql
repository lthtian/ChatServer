-- 直播卡片持久化传输方式，客户端据此选择桌面 RTMP 或摄像头 WebRTC。
ALTER TABLE LiveSession ADD COLUMN transport ENUM('rtmp','webrtc') NOT NULL DEFAULT 'rtmp';
