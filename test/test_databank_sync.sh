#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/zvcs"
BASE_A="$ROOT/test/databankA"
BASE_B="$ROOT/test/databankB"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

RUN="$TMPDIR/run"
WORK_A="$TMPDIR/databankA"
WORK_B="$TMPDIR/databankB"
mkdir -p "$RUN"
cp "$BIN" "$RUN/zvcs"
chmod +x "$RUN/zvcs"

cp -a "$BASE_A" "$WORK_A"
cp -a "$BASE_B" "$WORK_B"
rm -f "$WORK_A/peers.conf" "$WORK_B/peers.conf"
rm -f "$WORK_A/zvcs" "$WORK_A/zvcs.exe" "$WORK_B/zvcs" "$WORK_B/zvcs.exe"

cat > "$WORK_A/peers.conf" <<EOF
$WORK_B
EOF

cat > "$WORK_B/peers.conf" <<EOF
$WORK_A
EOF

cat > "$RUN/peers.conf" <<EOF
$WORK_B
EOF

python3 - "$WORK_A" "$WORK_B" <<'PY'
import os
import sqlite3
import sys

work_a, work_b = sys.argv[1], sys.argv[2]

def write_pdf(path, title):
    content = b"""%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /Type /Pages /Count 0 >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%EOF\n"""
    with open(path, "wb") as f:
        f.write(content)

def write_png(path):
    png = bytes.fromhex(
        "89504e470d0a1a0a0000000d4948445200000001000000010802000000907753de"
        "0000000a49444154789c6300010000050001"
        "0d0a2db40000000049454e44ae426082"
    )
    with open(path, "wb") as f:
        f.write(png)

def write_sqlite(path, note):
    conn = sqlite3.connect(path)
    cur = conn.cursor()
    cur.execute("create table if not exists items(id integer primary key, note text)")
    cur.execute("delete from items")
    cur.execute("insert into items(note) values (?)", (note,))
    conn.commit()
    conn.close()

write_pdf(os.path.join(work_a, "attachment.pdf"), "A")
write_png(os.path.join(work_a, "cover.png"))
write_sqlite(os.path.join(work_a, "library.sqlite"), "alpha")

write_pdf(os.path.join(work_b, "peer_attachment.pdf"), "B")
write_png(os.path.join(work_b, "peer_cover.png"))
write_sqlite(os.path.join(work_b, "peer_library.sqlite"), "beta")
PY

cd "$WORK_A"

sha_for_message() {
  "$RUN/zvcs" log | awk -F'|' -v msg="$1" '
    {
      gsub(/^[ \t]+|[ \t]+$/, "", $1);
      gsub(/^[ \t]+|[ \t]+$/, "", $3);
      if ($3 == msg) {
        print $1;
        exit 0;
      }
    }
  '
}

get_index_sha() {
  awk -v path="$1" '$2 == path { print $1; exit 0 }' .zvcs/index
}

"$RUN/zvcs" init >/dev/null
"$RUN/zvcs" check >/dev/null

"$RUN/zvcs" commit -m "baseline from databankA" >/dev/null
baseline_sha="$(sha_for_message "baseline from databankA")"
test -n "$baseline_sha"

status_clean="$("$RUN/zvcs" status)"
printf '%s\n' "$status_clean" | grep -q "working tree clean"

cat > filetest.txt <<'EOF'
databankA updated content
EOF
mkdir -p nested/level2
cat > nested/level2/note.txt <<'EOF'
deep file from databankA
EOF
"$RUN/zvcs" commit -m "databankA deep update" >/dev/null
deep_sha="$(sha_for_message "databankA deep update")"
test -n "$deep_sha"

printf 'modified after commit\n' > filetest.txt
mkdir -p scratch
printf 'untracked note\n' > scratch/new.txt
status_dirty="$("$RUN/zvcs" status)"
printf '%s\n' "$status_dirty" | grep -q "modified: filetest.txt"
printf '%s\n' "$status_dirty" | grep -q "untracked"

mkdir -p nested/level2
printf 'dirty overwrite\n' > filetest.txt
printf 'dirty nested\n' > nested/level2/note.txt
"$RUN/zvcs" checkout "$deep_sha" >/dev/null
grep -q "databankA updated content" filetest.txt
grep -q "deep file from databankA" nested/level2/note.txt

printf 'peer changed file\n' > "$WORK_B/peer_update.txt"
"$RUN/zvcs" fetch >/dev/null
grep -q "peer changed file" peer_update.txt

printf 'peer changed again\n' > "$WORK_B/peer_update.txt"
rm -f peer_update.txt
"$RUN/zvcs" fetch >/dev/null
grep -q "peer changed again" peer_update.txt

printf 'sqlite alpha updated\n' > "$WORK_B/library.sqlite"
printf 'new pdf from peer\n' > "$WORK_B/new_attachment.pdf"
printf 'new png from peer\n' > "$WORK_B/new_cover.png"
"$RUN/zvcs" fetch >/dev/null
grep -q "sqlite alpha updated" library.sqlite
test -f new_attachment.pdf
test -f new_cover.png

sqlite_a_sha="$(get_index_sha library.sqlite)"
pdf_a_sha="$(get_index_sha attachment.pdf)"
png_a_sha="$(get_index_sha cover.png)"
test -n "$sqlite_a_sha"
test -n "$pdf_a_sha"
test -n "$png_a_sha"

cp library.sqlite "$WORK_B/library.sqlite"
cp attachment.pdf "$WORK_B/peer_attachment.pdf"
cp cover.png "$WORK_B/peer_cover.png"

cat > "$WORK_A/editor.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
file="$1"
printf '%s\n' "# comment" "editor snapshot" "line two" >> "$file"
EOF
chmod +x "$WORK_A/editor.sh"
export EDITOR="$WORK_A/editor.sh"
printf 'editor change\n' >> filetest.txt
"$RUN/zvcs" commit >/dev/null
"$RUN/zvcs" log | grep -q "editor snapshot"
latest_two="$("$RUN/zvcs" log | head -n 2)"
printf '%s\n' "$latest_two" | grep -q "editor snapshot"
second_sha="$(sha_for_message "databankA deep update")"
test -n "$second_sha"

rm -f filetest.txt nested/level2/note.txt
"$RUN/zvcs" checkout "$baseline_sha" >/dev/null
grep -q "filetest.txt" .zvcs/index
test ! -e nested/level2/note.txt

dirty_sha="$(sha_for_message "editor snapshot line two")"
test -n "$dirty_sha"
printf 'dirty overwrite\n' > filetest.txt
printf 'dirty nested\n' > nested/level2/note.txt
"$RUN/zvcs" checkout "$dirty_sha" >/dev/null
printf 'editor snapshot\n' | grep -q "editor snapshot"

cat > "$RUN/peers.conf" <<EOF
$WORK_B
EOF

"$RUN/zvcs" check >/dev/null
check_out="$("$RUN/zvcs" check)"
printf '%s\n' "$check_out" | grep -q "\[ OK \]"

fetch_out="$("$RUN/zvcs" fetch)"
printf '%s\n' "$fetch_out" | grep -q "sync summary: copied"
printf '%s\n' "$fetch_out" | grep -q "sync classification: sqlite="
printf '%s\n' "$fetch_out" | grep -q "regular="

printf '/path/that/does/not/exist\n' > "$RUN/peers.conf"
if "$RUN/zvcs" check >/tmp/check.invalid 2>&1; then
  echo "expected invalid peers.conf entry to fail"
  exit 1
fi
grep -q "\[FAIL\]" /tmp/check.invalid

cat > "$RUN/peers.conf" <<'EOF'
EOF
if "$RUN/zvcs" check >/tmp/check.empty 2>&1; then
  echo "expected empty peers.conf to fail"
  exit 1
fi
grep -q "No peer paths found" /tmp/check.empty

grep -q "filetest.txt" .zvcs/index
grep -q "nested/level2/note.txt" .zvcs/index
grep -q "attachment.pdf" .zvcs/index
grep -q "cover.png" .zvcs/index
grep -q "library.sqlite" .zvcs/index
! grep -q "ignored/skip.bin" .zvcs/index

echo "databank sync tests passed"
