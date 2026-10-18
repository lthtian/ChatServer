#!/usr/bin/env bash
# 在云端安装已在本机编译、校验的产物；不执行编译。
set -euo pipefail
release_id=${1:?release id required}
[[ "$release_id" =~ ^[0-9]{8}-[0-9]{6}-media-jobs$ ]]
release=/home/lth/chat_server/releases/$release_id
backup=/home/lth/chat_server/backups/$release_id
dropin=/etc/systemd/system/chatserver.service.d/media-processing.conf
test "$(id -u)" = 0
test ! -e "$backup"
cd "$release"
sha256sum --check SHA256SUMS
for binary in ChatServer media_process; do
  file "$binary"
  dependencies=$(ldd "./$binary")
  printf '%s\n' "$dependencies"
  if [[ "$dependencies" == *"not found"* ]]; then exit 1; fi
done
test "$(mysql -NBe "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='chat' AND table_name='MediaJob'")" = 0
install -d -m 700 "$backup"
cp -a /etc/systemd/system/chatserver.service "$backup/"
cp -a /etc/systemd/system/chatserver.service.d "$backup/drop-ins"
systemctl cat chatserver.service > "$backup/effective-service.txt"

rollback() {
  if [[ -f "$backup/drop-ins/media-processing.conf" ]]; then
    cp "$backup/drop-ins/media-processing.conf" "$dropin"
  else
    rm -f "$dropin"
  fi
  systemctl daemon-reload
  systemctl restart chatserver.service
}
trap rollback ERR
systemctl stop chatserver.service
test "$(systemctl show chatserver.service -p MainPID --value)" = 0
umask 077
mysqldump --single-transaction --routines --events --triggers --hex-blob --no-tablespaces chat > "$backup/chat.sql"
tar -czf "$backup/media.tar.gz" -C /home/lth chat-media
test -s "$backup/chat.sql"
gzip -t "$backup/media.tar.gz"
mysql -NBe 'SELECT COUNT(*) FROM chat.History; SELECT COUNT(*) FROM chat.Media' > "$backup/counts-before.txt"
verify=chat_restore_${release_id//-/_}
mysql -e "CREATE DATABASE $verify CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
mysql "$verify" < "$backup/chat.sql"
mysql -NBe "SELECT COUNT(*) FROM $verify.History; SELECT COUNT(*) FROM $verify.Media" > "$backup/counts-restored.txt"
cmp "$backup/counts-before.txt" "$backup/counts-restored.txt"
mysql -e "DROP DATABASE $verify"
sha256sum "$backup/chat.sql" "$backup/media.tar.gz" > "$backup/SHA256SUMS"
mysql chat < 004_media_jobs.sql
chmod 755 ChatServer media_process
cat > "$dropin" <<EOF
[Service]
ExecStart=
ExecStart=$release/ChatServer 0.0.0.0 6000
Environment=CHAT_MEDIA_PROCESS=$release/media_process
Environment=CHAT_HLS_SEGMENT_TYPE=ts
EOF
chmod 644 "$dropin"
systemctl daemon-reload
systemctl start chatserver.service
systemctl is-active --quiet chatserver.service
trap - ERR
systemctl show chatserver.service -p ActiveState -p SubState -p MainPID -p NRestarts -p MemoryCurrent
mysql -NBe 'SELECT state,COUNT(*) FROM chat.MediaJob GROUP BY state'
printf 'release=%s\nbackup=%s\n' "$release" "$backup"
