#!/usr/bin/env bash
# 安装本机产物和 RTC 配置；失败时恢复服务配置，保留已完成的加字段迁移。
set -euo pipefail
release_id=${1:?release id required}
[[ "$release_id" =~ ^[0-9]{8}-[0-9]{6}-rtc$ ]]
release=/home/lth/chat_server/releases/$release_id
backup=/home/lth/chat_server/backups/$release_id
root=/home/lth/chat-live
test "$(id -u)" = 0
test ! -e "$backup"
test -f "$root/control.env"
cd "$release"
sha256sum --check SHA256SUMS
for binary in ChatServer srs; do
    chmod 755 "$binary"
    dependencies=$(ldd "./$binary")
    [[ "$dependencies" != *"not found"* ]]
done
install -d -m 700 "$backup"
cp -a /etc/systemd/system/chatserver.service.d/zz-live.conf "$backup/chatserver-live.conf"
cp -a /etc/systemd/system/srs-chat.service "$backup/srs-chat.service"
cp -a /etc/nginx/snippets/chat-live.conf "$backup/nginx-live.conf"
cp -a "$root/srs.conf" "$backup/srs.conf"
umask 077
mysqldump --single-transaction --routines --events --triggers --hex-blob --no-tablespaces chat > "$backup/chat.sql"
test -s "$backup/chat.sql"
sha256sum "$backup/chat.sql" > "$backup/SHA256SUMS"
rollback() {
    trap - ERR
    echo "RTC deployment failed; restoring service configuration" >&2
    cp -a "$backup/chatserver-live.conf" /etc/systemd/system/chatserver.service.d/zz-live.conf
    cp -a "$backup/srs-chat.service" /etc/systemd/system/srs-chat.service
    cp -a "$backup/nginx-live.conf" /etc/nginx/snippets/chat-live.conf
    cp -a "$backup/srs.conf" "$root/srs.conf"
    systemctl daemon-reload
    systemctl restart chatserver.service srs-chat.service
    nginx -t && systemctl reload nginx
}
trap rollback ERR
# 沿用已有回调密钥，配置与环境文件保持一致；密钥不出现在命令输出中。
secret=$(sed -n 's/^CHAT_LIVE_HOOK_SECRET=//p' "$root/control.env")
[[ "$secret" =~ ^[0-9a-f]{64}$ ]]
sed -e "s|__LIVE_ROOT__|$root/hls|g" -e "s|__HOOK_SECRET__|$secret|g" srs-chat.conf.in > "$root/srs.conf"
chown lth:lth "$root/srs.conf"
chmod 600 "$root/srs.conf"
sudo -u lth "$release/srs" -t -c "$root/srs.conf" > "$backup/srs-config-check.log" 2>&1
if [[ $(mysql -NBe "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema='chat' AND table_name='LiveSession' AND column_name='transport'") = 0 ]]; then
    mysql chat < 006_live_transport.sql
fi
sed "s|__RELEASE__|$release|g" srs-chat.service > /etc/systemd/system/srs-chat.service
sed "s|__RELEASE__|$release|g" chatserver-live.conf > /etc/systemd/system/chatserver.service.d/zz-live.conf
install -m 644 nginx-chat-live.conf /etc/nginx/snippets/chat-live.conf
nginx -t
systemctl daemon-reload
systemctl restart chatserver.service srs-chat.service
systemctl reload nginx
sleep 2
systemctl is-active --quiet chatserver.service srs-chat.service
curl -fsS http://127.0.0.1:1985/api/v1/versions
trap - ERR
systemctl show chatserver.service srs-chat.service -p ActiveState -p MainPID -p NRestarts -p MemoryCurrent
printf '\nrelease=%s\nbackup=%s\n' "$release" "$backup"
