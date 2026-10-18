#!/usr/bin/env bash
# 仅安装本机编译产物，先备份并验证数据库恢复，再切换运行版本。
set -euo pipefail
release_id=${1:?release id required}
[[ "$release_id" =~ ^[0-9]{8}-[0-9]{6}-live$ ]]
release=/home/lth/chat_server/releases/$release_id
backup=/home/lth/chat_server/backups/$release_id
root=/home/lth/chat-live
test "$(id -u)" = 0
test ! -e "$backup"
test ! -e "$root"
cd "$release"
sha256sum --check SHA256SUMS
for binary in ChatServer srs; do
    dependencies=$(ldd "./$binary")
    [[ "$dependencies" != *"not found"* ]]
done
test "$(mysql -NBe "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='chat' AND table_name='LiveSession'")" = 0
install -d -m 700 "$backup"
cp -a /etc/systemd/system/chatserver.service "$backup/"
cp -a /etc/systemd/system/chatserver.service.d "$backup/drop-ins"
cp -a /etc/nginx/conf.d/chat-media.conf "$backup/"
rollback() {
    trap - ERR
    echo "Live deployment failed; restoring previous service configuration" >&2
    systemctl stop srs-chat.service || true
    systemctl disable srs-chat.service || true
    rm -f /etc/systemd/system/chatserver.service.d/zz-live.conf
    cp -a "$backup/chat-media.conf" /etc/nginx/conf.d/chat-media.conf
    systemctl daemon-reload
    systemctl restart chatserver.service
    nginx -t && systemctl reload nginx
    # 已成功执行的扩展表迁移保留，不覆盖部署后数据。
}
trap rollback ERR
systemctl stop chatserver.service
test "$(systemctl show chatserver.service -p MainPID --value)" = 0
umask 077
mysqldump --single-transaction --routines --events --triggers --hex-blob --no-tablespaces chat > "$backup/chat.sql"
tar -czf "$backup/media.tar.gz" -C /home/lth chat-media
gzip -t "$backup/media.tar.gz"
mysql -NBe 'SELECT COUNT(*) FROM chat.History; SELECT COUNT(*) FROM chat.Media' > "$backup/counts-before.txt"
verify=chat_restore_${release_id//-/_}
mysql -e "CREATE DATABASE $verify CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
mysql "$verify" < "$backup/chat.sql"
mysql -NBe "SELECT COUNT(*) FROM $verify.History; SELECT COUNT(*) FROM $verify.Media" > "$backup/counts-restored.txt"
cmp "$backup/counts-before.txt" "$backup/counts-restored.txt"
mysql -e "DROP DATABASE $verify"
sha256sum "$backup/chat.sql" "$backup/media.tar.gz" > "$backup/SHA256SUMS"

install -d -o lth -g lth -m 700 "$root" "$root/hls"
secret=$(openssl rand -hex 32)
sed -e "s|__LIVE_ROOT__|$root/hls|g" -e "s|__HOOK_SECRET__|$secret|g" srs-chat.conf.in > "$root/srs.conf"
printf 'CHAT_LIVE_HOOK_SECRET=%s\n' "$secret" > "$root/control.env"
chown lth:lth "$root/srs.conf" "$root/control.env"
chmod 600 "$root/srs.conf" "$root/control.env"
chmod 755 ChatServer srs
sudo -u lth "$release/srs" -t -c "$root/srs.conf" > "$backup/srs-config-check.log" 2>&1
mysql chat < 005_live_sessions.sql
install -m 644 srs-chat.service /etc/systemd/system/srs-chat.service
sed -i "s|__RELEASE__|$release|g" /etc/systemd/system/srs-chat.service
install -m 644 chatserver-live.conf /etc/systemd/system/chatserver.service.d/zz-live.conf
sed -i "s|__RELEASE__|$release|g" /etc/systemd/system/chatserver.service.d/zz-live.conf
install -d -m 755 /etc/nginx/snippets
install -m 644 nginx-chat-live.conf /etc/nginx/snippets/chat-live.conf
install -m 644 nginx-chat-media.conf /etc/nginx/conf.d/chat-media.conf
nginx -t
systemctl daemon-reload
systemctl enable --now srs-chat.service
systemctl start chatserver.service
systemctl is-active --quiet chatserver.service srs-chat.service
systemctl reload nginx
trap - ERR
systemctl show chatserver.service srs-chat.service -p ActiveState -p MainPID -p NRestarts -p MemoryCurrent
printf 'release=%s\nbackup=%s\n' "$release" "$backup"
